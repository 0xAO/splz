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

typedef struct {
	int sock;
}onconnect_loop_param_t;

forPlugin_t * addition_plg;

uint8_t** layer_buffers=NULL;
const char* layer_names[]={LAYER_L0_MUXER_NAME,LAYER_WEB_NAME,0};
uint8_t* output_buffer=NULL;

pthread_mutex_t tcp_outclck_mtx;

long tcp_need_send_packets=0;

void* tcp_output_update_timer(void* p){
	while(true){
		pthread_mutex_lock(&tcp_outclck_mtx);
		tcp_need_send_packets++;
		pthread_mutex_unlock(&tcp_outclck_mtx);
		usleep(1000000/24);
	}
}


int getLayerIdFromName(const char* n){
	for(int i=0;layer_names[i];i++){
		if(strcmp(layer_names[i],n)==0){return i;}
	}
	return -1;
}

void* onconnect_loop(void* p_){
	printf("onconnect_loop a\n");
	onconnect_loop_param_t * p =(onconnect_loop_param_t*)p_;
	char hello[1024];
	sprintf(hello,"HELLO ALL_Muxer. FROM [%08d]: ",0);
	int hello_len=strlen(hello);
	if(recv_all(p->sock,hello,hello_len)){
		printf("hello received. text='%s'\n",hello);
		unsigned int layer_name_len;
		if(sscanf(hello,"HELLO ALL_Muxer. FROM [%08d]: ",&layer_name_len)==1 && layer_name_len>0 && layer_name_len<=32){
			printf("sscanf ret ok la len = %d\n",layer_name_len);
			char layer_name[128];
			if(recv_all(p->sock,layer_name,layer_name_len+1 /* name + dot */) && layer_name[layer_name_len]=='.'){
				layer_name[layer_name_len]=0;
				printf("connected layer %s\n",layer_name);
				int lid=getLayerIdFromName(layer_name);
				printf("layer id = %d\n",lid);
				uint8_t * buff=(lid==-1)?NULL:layer_buffers[lid];
				if(buff){
					printf("buff found and not null\n");
					for(;;){
						if(recv_all(p->sock,buff,VIDEO_WIDTH*VIDEO_HEIGHT*4)){

						}else{
							break;
						}
					}
				}
			}
		}
	}
	printf("closing and exiting from thread\n");
	close(p->sock);
	delete p;
	return NULL;
}


void* receiver_loop(void* p){
	int listener=create_listener(PORT_ALLMUX_IN);
	struct sockaddr_in cli_sin;
	socklen_t cli_sl;
	int acc_s=-1;
	for(;;){
		acc_s=accept(listener,(struct sockaddr *)&cli_sin, (socklen_t*)&cli_sl);
		if(acc_s!=-1){
			pthread_t onconnect_loop_pt;
			onconnect_loop_param_t * p=new onconnect_loop_param_t;
			p->sock=acc_s;
			pthread_create(&onconnect_loop_pt,NULL,onconnect_loop,(void*)p);
			printf("accepted(receiver_loop). sid=%d\n",acc_s);
		}else{
			perror("accept(receiver_loop) error");
		}
	}
}




void* sender_thread(void* p){
	printf("hello from sender thread\n");
	int sd;
	while( ( sd=create_client_and_connect(PORT_ALLMUX_OUT,1))!=-1 ){
		tcp_need_send_packets=0;
		for(;;){	
			bool yes_send_packet=false;
			pthread_mutex_lock(&tcp_outclck_mtx);
			if(tcp_need_send_packets){tcp_need_send_packets--;yes_send_packet=true;}
			pthread_mutex_unlock(&tcp_outclck_mtx);
			if(yes_send_packet){
				alpha_blend_picture(layer_buffers[0],layer_buffers[1],output_buffer);
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


int main(){
	signal(SIGPIPE,SIG_IGN);
	pthread_t receiver_loop_pt;
	pthread_t sender_thread_pt;
	pthread_t tcp_outclck_t;

	int layers_count=0;
	for(int i=0;layer_names[i];i++){
		layers_count++;
	}
	
	layer_buffers=(uint8_t**)malloc(sizeof(uint8_t*)*(layers_count+1)); //+1 for non-sigfault if lc=0 :)
	memset(layer_buffers,0x00,sizeof(uint8_t*)*(layers_count+1));
	
	addition_plg=createForPluginByName("addition_alpha",VIDEO_WIDTH,VIDEO_HEIGHT,"");
	if(!addition_plg){
		printf("cannot create plugin addition_alpha\n");
		return 0;
	}

	for(int i=0;layer_names[i];i++){
		layer_buffers[i]=(uint8_t*)malloc(VIDEO_WIDTH*VIDEO_HEIGHT*4);
		if(!layer_buffers[i]){
			perror("can't allocate memory for layer_buffers");
			exit(0);
		}
	}
	
	output_buffer=(uint8_t*)malloc(VIDEO_WIDTH*VIDEO_HEIGHT*4);
	if(!output_buffer){
		perror("can't allocate memory for output_buffer");
		exit(0);		
	}


	pthread_mutex_init(&tcp_outclck_mtx,NULL);
	pthread_create(&tcp_outclck_t,NULL,tcp_output_update_timer,NULL);

	pthread_create(&receiver_loop_pt,NULL,receiver_loop,NULL);
	pthread_create(&sender_thread_pt,NULL,sender_thread,NULL);


	for(;;){sleep(1);}
}
