#include <sys/socket.h>
#include <netinet/in.h>
#include <stdlib.h>
#include <arpa/inet.h>
#include <signal.h>
#include <pthread.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>
#include "../../libs/config/config.h"
#include "../../libs/forlib/myforlib.h"
#include "../../libs/tcp/tcp.h"
#include "../../libs/picture/picture.h"

bool _param_process_green=true;
bool _param_process_bluralpha=true;
bool _param_process_background=true;

uint8_t* raw_frame;//rgba in from camera
uint8_t* raw_greenmask; //mask image ( IMAGE!!! RGBA EPTA )
uint8_t* raw_background; //rgba
forPlugin_t * addition_plg;

enum {MODE_NORMAL,MODE_MAKING_GREEN};
enum chroma_parameter_type_t {CHROMA_PARAMETER_RGB,CHROMA_PARAMETER_IMAGEMASK};

int mode=MODE_NORMAL;

#define CHROMA_RGB_RED 0x01000000
#define CHROMA_RGB_GREEN 0x02000000
#define CHROMA_RGB_BLUE 0x03000000

pthread_mutex_t tcp_outclck_mtx,raw_frame_mtx;


inline uint8_t pix_diff(uint8_t a,uint8_t b){
	uint8_t r=(a>b)?(a-b):(b-a);
	if(r==0){r=1;}
	return r;
}

void process_frame(chroma_parameter_type_t chroma_pt, long chroma_param,uint8_t chroma_max_diff,uint8_t chroma_prev_diff){

uint8_t chroma_pr_r,chroma_pr_g,chroma_pr_b;
uint8_t* srcptr=raw_frame;
uint8_t* imgmaskptr=raw_greenmask;

uint8_t chroma_main_color=((uint8_t*)&chroma_param)[3]; //1=r,2=g,3=b

uint8_t chroma_r=(chroma_pt==CHROMA_PARAMETER_RGB)?((uint8_t*)&chroma_param)[2]:0;
uint8_t chroma_g=(chroma_pt==CHROMA_PARAMETER_RGB)?((uint8_t*)&chroma_param)[1]:0;
uint8_t chroma_b=(chroma_pt==CHROMA_PARAMETER_RGB)?((uint8_t*)&chroma_param)[0]:0;


	for(int y=0;y<VIDEO_HEIGHT;y++){
		for(int x=0;x<VIDEO_WIDTH;x++){
			uint8_t r,g,b,a;
			uint8_t * a_ptr;
			r=*(srcptr++);
			g=*(srcptr++);
			b=*(srcptr++);
			a_ptr=srcptr; //will be mask
			a=*(srcptr++);
			bool set_mask=false;
		
			uint8_t c_chroma_diffv1,c_chroma_diffv2;

			if(chroma_pt==CHROMA_PARAMETER_RGB){
				uint8_t diff_r=pix_diff(chroma_r,r);
				uint8_t diff_g=pix_diff(chroma_g,g);
				uint8_t diff_b=pix_diff(chroma_b,b);
				int all_diff=0;
				switch(chroma_main_color){
					case 0x01: all_diff=(diff_r)+(diff_g*2)+(diff_b*2);break;
					case 0x02: all_diff=(diff_r*2)+(diff_g)+(diff_b*2);break;
					case 0x03: all_diff=(diff_r*2)+(diff_g*2)+(diff_b);break;
					default:all_diff=diff_r+diff_g+diff_b;break;
				}
				if(all_diff<chroma_max_diff){set_mask=true;}
			}else if(chroma_pt=CHROMA_PARAMETER_IMAGEMASK){
				chroma_r=*(imgmaskptr++);
				chroma_g=*(imgmaskptr++);
				chroma_b=*(imgmaskptr++);
				imgmaskptr++; //alfa
			
				uint8_t diff_r=pix_diff(chroma_r,r);
				uint8_t diff_g=pix_diff(chroma_g,g);
				uint8_t diff_b=pix_diff(chroma_b,b);
				int all_diff=(diff_r+diff_g+diff_b);
				if(all_diff<chroma_max_diff){set_mask=true;}
			}
		
			*a_ptr=set_mask?0x00:0xFF; //alpha

			if(set_mask){
				chroma_pr_r=r;
				chroma_pr_g=g;
				chroma_pr_b=b;
			}
		}//for W
	}//for H
}//func


void blur_mask(){
	return;
}


long tcp_need_send_packets=0;

void* tcp_output_update_timer(void* p){
	for(;;){
		pthread_mutex_lock(&tcp_outclck_mtx);
		tcp_need_send_packets++;
		pthread_mutex_unlock(&tcp_outclck_mtx);
		usleep(1000000/24);
	}
}

void* tcp_input_bg_th(void* p){
	int listener=create_listener(PORT_L0_BACKGROUND_IN);
	struct sockaddr_in cli_sin;
	socklen_t cli_sl;
	int acc_s=-1;
	for(;;){
		if(acc_s!=-1){
			if(!recv_all(acc_s,raw_background,VIDEO_WIDTH*VIDEO_HEIGHT*4)){perror("recv err");close(acc_s);acc_s=-1;}
		}else{
			cli_sl=sizeof(sockaddr_in);
			acc_s=accept(listener,(struct sockaddr *)&cli_sin, (socklen_t*)&cli_sl);
			if(acc_s==-1){perror("accept(tcp_input_th) error");}else{printf("accepted(tcp_input_bg_th). sid=%d\n",acc_s);}
		}
	}
}

void* tcp_input_th(void* p){
	uint8_t* recv_buffer=(uint8_t* )malloc(VIDEO_WIDTH*VIDEO_HEIGHT*4);
	int listener=create_listener(PORT_L0_CAMERA_IN);
	struct sockaddr_in cli_sin;
	socklen_t cli_sl;
	int acc_s=-1;
	for(;;){
		if(acc_s!=-1){
			if(recv_all(acc_s,recv_buffer,VIDEO_WIDTH*VIDEO_HEIGHT*4)){
				pthread_mutex_lock(&raw_frame_mtx);
				if(mode==MODE_NORMAL){
					memcpy(raw_frame,recv_buffer,VIDEO_WIDTH*VIDEO_HEIGHT*4);
				}else if(mode==MODE_MAKING_GREEN){
					memcpy(raw_greenmask,recv_buffer,VIDEO_WIDTH*VIDEO_HEIGHT*4);
				}
				pthread_mutex_unlock(&raw_frame_mtx);
			}else{
				perror("recv err");
				close(acc_s);
				acc_s=-1;
			}
		}else{
			cli_sl=sizeof(sockaddr_in);
			acc_s=accept(listener,(struct sockaddr *)&cli_sin, (socklen_t*)&cli_sl);
			if(acc_s==-1){perror("accept(tcp_input_th) error");}else{printf("accepted(tcp_output_th). sid=%d\n",acc_s);}
		}
	}
}
void* tcp_output_th(void* p){
	int sd;
	uint8_t* send_buffer=(uint8_t* )malloc(VIDEO_WIDTH*VIDEO_HEIGHT*4);
	while( ( sd=create_client_and_connect(PORT_L0_OUT,1))!=-1 ){
		tcp_need_send_packets=0;
		char hello[1024];
		sprintf(hello,"HELLO ALL_Muxer. FROM [%08d]: %s.",strlen(LAYER_L0_MUXER_NAME),LAYER_L0_MUXER_NAME);
		send_all(sd,hello,strlen(hello));
		for(;;){
			bool yes_send_packet=false;
			pthread_mutex_lock(&tcp_outclck_mtx);
			if(tcp_need_send_packets){tcp_need_send_packets--;yes_send_packet=true;}
			pthread_mutex_unlock(&tcp_outclck_mtx);
			if(yes_send_packet){
				pthread_mutex_lock(&raw_frame_mtx);
				if(_param_process_background){
					if(_param_process_green){
						process_frame(CHROMA_PARAMETER_IMAGEMASK,0,25,5); //making mask. 
						if(_param_process_bluralpha){
							blur_mask();
						}
					}
					alpha_blend_picture(raw_background,raw_frame,send_buffer);
				}else{
					memcpy(send_buffer,raw_frame,VIDEO_WIDTH*VIDEO_HEIGHT*4);			
				}
				pthread_mutex_unlock(&raw_frame_mtx);
				if(! send_all(sd,send_buffer,VIDEO_WIDTH*VIDEO_HEIGHT*4) ){
					perror("sending err");
					close(sd);
					sd=-1;
					break;// because it's CLIENT. on client breaking.
				}
			}else{
				usleep(10000);
			}
		}//for ;; (while need send);
		sleep(1);
	}//on conn (while)
}//func

int main(){
	signal(SIGPIPE,SIG_IGN);

	addition_plg=createForPluginByName("addition_alpha",VIDEO_WIDTH,VIDEO_HEIGHT,"");
	if(!addition_plg){
		printf("cannot create plugin addition_alpha\n");
		return 0;
	}

	raw_frame=(uint8_t*)malloc(VIDEO_WIDTH*VIDEO_HEIGHT*3*4);
	raw_greenmask=(uint8_t*)malloc(VIDEO_WIDTH*VIDEO_HEIGHT*4);
	raw_background=(uint8_t*)malloc(VIDEO_WIDTH*VIDEO_HEIGHT*4);

	if(!raw_frame || !raw_greenmask || !raw_background){
		printf("cannot allocate buffers\n");
		return 0;
	}

	memset(raw_frame,0x00,sizeof(VIDEO_WIDTH*VIDEO_HEIGHT*4));
	memset(raw_greenmask,0x00,sizeof(VIDEO_WIDTH*VIDEO_HEIGHT*4));
	memset(raw_background,0x00,sizeof(VIDEO_WIDTH*VIDEO_HEIGHT*4));

	pthread_t tcp_out_t,tcp_in_t,tcp_outclck_t,tcp_in_bg_t;
	pthread_create(&tcp_out_t,NULL,tcp_output_th,NULL);
	pthread_create(&tcp_in_t,NULL,tcp_input_th,NULL);
	pthread_create(&tcp_in_bg_t,NULL,tcp_input_bg_th,NULL);
	pthread_create(&tcp_outclck_t,NULL,tcp_output_update_timer,NULL);
	pthread_mutex_init(&tcp_outclck_mtx,NULL);
	pthread_mutex_init(&raw_frame_mtx,NULL);

	while(!feof(stdin)){
		char buff[1024];
		fgets(buff,1024,stdin);
		buff[1023]=0;
		if(strcmp(buff,"mg\n")==0){
			printf("making green\n");
			mode=MODE_MAKING_GREEN;
		}else if(strcmp(buff,"do\n")==0){
			printf("DOing\n");
			mode=MODE_NORMAL;
		}else if(strcmp(buff,"exit\n")==0){
			exit(0);
		}
	}
}
