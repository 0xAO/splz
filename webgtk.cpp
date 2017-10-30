#include <gtk/gtk.h>
#include <webkit2/webkit2.h>
#include <pthread.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <stdlib.h>
#include <arpa/inet.h>
#include <signal.h>
#include "../../libs/config/config.h"
#include "../../libs/tcp/tcp.h"

uint8_t* output_buffer=NULL;

int tcp_need_send_packets=0;

pthread_mutex_t tcp_outclck_mtx;

static void destroyWindowCb(GtkWidget* widget, GtkWidget* window);
static gboolean closeWebViewCb(WebKitWebView* webView, GtkWidget* window);

bool downed=false;

GtkWidget *main_window=NULL;
WebKitWebView *webView=NULL;
bool need_update_buffer=false;
unsigned char* image_buffer=NULL;


static gboolean on_draw_event(GtkWidget *widget, cairo_t *cr, gpointer user_data) {
	need_update_buffer=true;
	return 0;
}

static void load_changed(WebKitWebView  *web_view, WebKitLoadEvent load_event, gpointer user_data) {
	printf(" downloading status=%d %s\n",load_event, webkit_web_view_get_uri(web_view));
	if(load_event==3){
		downed=true;
	}
}

void copy_hdpi_to_normal(uint8_t* src_,uint8_t* dest_){
	uint32_t * src=(uint32_t*)src_;
	uint32_t * dest=(uint32_t*)dest_;
	for(int y=0;y<VIDEO_HEIGHT;y++){
		for(int x=0;x<VIDEO_WIDTH;x++){
			*(dest++)=*(src++);
			src++;
		}
		src+=(VIDEO_WIDTH*2);
	}
}

void update_buffer(){
	if(!need_update_buffer){
		return;
	}
	need_update_buffer=false;
	GdkPixbuf *pb = gdk_pixbuf_get_from_window(gtk_widget_get_window (GTK_WIDGET(webView)),0,0,VIDEO_WIDTH,VIDEO_HEIGHT);
	if(pb != NULL) {
		uint8_t* pixels=(uint8_t*)gdk_pixbuf_get_pixels (pb);
		printf("update buffer (%d x %d (%d))bps=%d ha=%d \n", gdk_pixbuf_get_width(pb),gdk_pixbuf_get_height(pb),gdk_pixbuf_get_byte_length(pb),gdk_pixbuf_get_bits_per_sample (pb),gdk_pixbuf_get_has_alpha (pb));
		bool hdpi=(gdk_pixbuf_get_width(pb)/2 == VIDEO_WIDTH);
		if(hdpi){
			copy_hdpi_to_normal(pixels,output_buffer);
		}else{
			memcpy(output_buffer,pixels,VIDEO_WIDTH*VIDEO_HEIGHT*4);
		}
//		gdk_pixbuf_unref(pb);
		g_object_unref(pb);
	}
}

void* tcp_output_update_timer(void* p){
	while(true){
		pthread_mutex_lock(&tcp_outclck_mtx);
		tcp_need_send_packets++;
		pthread_mutex_unlock(&tcp_outclck_mtx);
		usleep(1000000/WEBVIEW_FPS);
	}
}

void* sender_thread(void* p){
	printf("hello from sender thread\n");
	int sd;
	while( ( sd=create_client_and_connect(PORT_WEBVIEW_OUT,1))!=-1 ){
		char hello[1024];
		sprintf(hello,"HELLO ALL_Muxer. FROM [%08d]: %s.",strlen(LAYER_WEB_NAME),LAYER_WEB_NAME);
		send_all(sd,hello,strlen(hello));
		tcp_need_send_packets=0;
		for(;;){
			bool yes_send_packet=false;
			pthread_mutex_lock(&tcp_outclck_mtx);
			if(tcp_need_send_packets){tcp_need_send_packets--;yes_send_packet=true;}
			pthread_mutex_unlock(&tcp_outclck_mtx);
			if(yes_send_packet){
				update_buffer();
				if(!send_all(sd,output_buffer,VIDEO_WIDTH*VIDEO_HEIGHT*4)){
					break;				
				}
			}else{
				usleep(10000);
			}
		}//for ;; 
		sleep(1);		
	}
	printf("exit from sender thread\n");
}



int main(int argc, char* argv[])
{
	signal(SIGPIPE,SIG_IGN);

	output_buffer=(uint8_t*)malloc(VIDEO_WIDTH*VIDEO_HEIGHT*4);
	if(!output_buffer){
		perror("can't allocate memory for output_buffer");
		exit(0);		
	}

	// Initialize GTK+
	gtk_init(&argc, &argv);
	main_window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
	gtk_window_set_default_size(GTK_WINDOW(main_window), VIDEO_WIDTH, VIDEO_HEIGHT);
	webView = WEBKIT_WEB_VIEW(webkit_web_view_new());
	g_signal_connect(G_OBJECT(webView), "load-changed", G_CALLBACK(load_changed), NULL);
	g_signal_connect(G_OBJECT(webView), "draw", G_CALLBACK(on_draw_event), NULL); 
	gtk_container_add(GTK_CONTAINER(main_window), GTK_WIDGET(webView));
	g_signal_connect(main_window, "destroy", G_CALLBACK(destroyWindowCb), NULL);
	g_signal_connect(webView, "close", G_CALLBACK(closeWebViewCb), main_window);

	GdkRGBA GdkRGBAn = GdkRGBA{1.0,1.0,1.0,0.0};
	webkit_web_view_set_background_color (webView,&GdkRGBAn);
	webkit_web_view_load_uri(webView, "https://0x.gy/internaluse/streamplatz/");
	gtk_widget_grab_focus(GTK_WIDGET(webView));
	gtk_widget_show_all(main_window);

	pthread_t sender_thread_t,tcp_outclck_t;
	pthread_mutex_init(&tcp_outclck_mtx,NULL);
	pthread_create(&tcp_outclck_t,NULL,tcp_output_update_timer,NULL);
	pthread_create(&sender_thread_t,NULL,sender_thread,NULL);

	gtk_main();

	return 0;
}


static void destroyWindowCb(GtkWidget* widget, GtkWidget* window)
{
	gtk_main_quit();
}

static gboolean closeWebViewCb(WebKitWebView* webView, GtkWidget* window)
{
	gtk_widget_destroy(window);
	return TRUE;
}
