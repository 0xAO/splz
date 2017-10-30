#include <errno.h>
#include <unistd.h>
#include <malloc.h>
#include <string.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <resolv.h>
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <signal.h>

extern "C"{
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/common.h>
#include <libavutil/imgutils.h>
#include <libavutil/mathematics.h>
#include <libavutil/samplefmt.h>
#include <libswscale/swscale.h>
}

#include <pthread.h>
#include "../../libs/config/config.h"
#include "../../libs/tcp/tcp.h"


uint8_t * received_picture_rgb32;

SSL_CTX *ssl_ctx;

SSL_CTX* InitServerCTX(void)
{
	SSL_METHOD *method;
	SSL_CTX *ctx;

	OpenSSL_add_all_algorithms();
	SSL_load_error_strings();

	method = (SSL_METHOD*)TLSv1_2_server_method();
	ctx = SSL_CTX_new(method);
	if ( ctx == NULL )
	{
		ERR_print_errors_fp(stderr);
		abort();
	}
	return ctx;
}

void LoadCertificates(SSL_CTX* ctx, const char* CertFile, const char* KeyFile)
{
	if (SSL_CTX_load_verify_locations(ctx, CertFile, KeyFile) != 1){
		ERR_print_errors_fp(stderr);
	}
	if (SSL_CTX_set_default_verify_paths(ctx) != 1){
		ERR_print_errors_fp(stderr);
	}
	if (SSL_CTX_use_certificate_file(ctx, CertFile, SSL_FILETYPE_PEM) <= 0)
	{
		ERR_print_errors_fp(stderr);
		abort();
	}
	if (SSL_CTX_use_PrivateKey_file(ctx, KeyFile, SSL_FILETYPE_PEM) <= 0)
	{
		ERR_print_errors_fp(stderr);
		abort();
	}
	if (!SSL_CTX_check_private_key(ctx))
	{
		fprintf(stderr, "Private key does not match the public certificate\n");
		abort();
	}
}


typedef struct {
	uint8_t * receiver_buffer;
	SSL  * ssl;
	AVCodec *av_codec;
	AVCodecContext *av_ctx;
	AVFrame *av_frame;
	AVFrame* av_scaled_frame;
	AVPacket av_pkt;
	pthread_t thread;
	struct SwsContext *img_convert_ctx;
}servlet_data_t;

void free_servlet_data(servlet_data_t* sd){
	if(sd==NULL){return;}
	if(sd->ssl!=NULL){
		int sock = SSL_get_fd(sd->ssl);
		SSL_free(sd->ssl);
		close(sock);
	}

	if(sd->av_ctx){
		avcodec_close(sd->av_ctx);
		av_free(sd->av_ctx);
	}

	sd->av_pkt.data = NULL;
	sd->av_pkt.size = 0;

	if(sd->av_frame){
		av_frame_free(&sd->av_frame);
	}

	if(sd->av_scaled_frame){
		av_frame_free(&sd->av_scaled_frame);
	}
	
	memset(sd,0x00,sizeof(servlet_data_t));
}

servlet_data_t * create_servlet_data(int fd){
	servlet_data_t * sd=(servlet_data_t*)malloc( sizeof(servlet_data_t));
	if(sd==NULL){return NULL;}
	memset(sd,0x00,sizeof(servlet_data_t));

	sd->receiver_buffer=(uint8_t*)malloc(512000); //512kb
	if(sd->receiver_buffer==NULL){
		free_servlet_data(sd);
		return NULL;
	}
	sd->ssl = SSL_new(ssl_ctx);
	SSL_set_fd(sd->ssl, fd);

	av_init_packet(&sd->av_pkt);

	sd->av_codec = avcodec_find_decoder(AV_CODEC_ID_VP8 );
	if (!sd->av_codec) {
		fprintf(stderr, "Codec not found\n");
		free_servlet_data(sd);
		return NULL;
	}

	sd->av_ctx = avcodec_alloc_context3(sd->av_codec);
	if (!sd->av_ctx) {
		fprintf(stderr, "Could not allocate video codec context\n");
		free_servlet_data(sd);
		return NULL;
	}

	if (sd->av_codec->capabilities & AV_CODEC_CAP_TRUNCATED){
		sd->av_ctx->flags |= AV_CODEC_FLAG_TRUNCATED;
	}


	if (avcodec_open2(sd->av_ctx, sd->av_codec, NULL) < 0) {
		fprintf(stderr, "Could not open codec\n");
		free_servlet_data(sd);
		return NULL;
	}

	sd->av_frame = av_frame_alloc();

	if (!sd->av_frame) {
		fprintf(stderr, "Could not allocate video frame\n");
		free_servlet_data(sd);
		return NULL;
	}

	sd->av_scaled_frame=av_frame_alloc();
	if (!sd->av_scaled_frame) {
		fprintf(stderr, "Could not allocate scaled_frame\n");
		free_servlet_data(sd);
		return NULL;
	}
	sd->av_scaled_frame->format=AV_PIX_FMT_RGB32;
	sd->av_scaled_frame->width=VIDEO_WIDTH;
	sd->av_scaled_frame->height=VIDEO_HEIGHT;

	int ret = av_image_alloc(
		sd->av_scaled_frame->data,
		sd->av_scaled_frame->linesize,
		sd->av_scaled_frame->width,
		sd->av_scaled_frame->height,
		(AVPixelFormat)sd->av_scaled_frame->format,
		32 );
	if (ret < 0) {
		fprintf(stderr, "Could not allocate raw picture buffer\n");
		free_servlet_data(sd);
		return NULL;
	}

	return sd;
}

void avcodec_decode_video_notdeprecated (AVCodecContext *avctx, AVFrame *frame, int *got_picture_ptr, const AVPacket *avpkt){
	int a;
	for(;;){
		a=avcodec_send_packet(avctx,avpkt);
		if(a!=0){
			if(a==AVERROR(EAGAIN)){
				printf("Warning! EAGAIN\n");
				usleep(10000);
			}else{
				*got_picture_ptr=0;
				return;
			}
		}else{
			break;
		}
	}
	//f2
	for(;;){
		a=avcodec_receive_frame(avctx,frame);
		if(a!=0){
			if(a==AVERROR(EAGAIN)){
				printf("Warning! EAGAIN\n");
				usleep(10000);
			}else{
				*got_picture_ptr=0;
				return;
			}
		}else{
			break;
		}
	}
	*got_picture_ptr=1;
}


void* Servlet(void* dat_) {
	servlet_data_t * sd=(servlet_data_t*)dat_;

	size_t received_bytes;
	char info_buff[11];
	unsigned int info_dat_len;
	char info_dat_type[3];

	info_buff[10]=0;
	info_dat_type[2]=0;

	if ( SSL_accept(sd->ssl) == -1 ){ 
		ERR_print_errors_fp(stderr);
		free_servlet_data(sd);
		return NULL;
	}else{
		for(;;){
			received_bytes = SSL_read(sd->ssl, info_buff, 10);
			if(received_bytes!=10){
				break;
			}
			if(sscanf(info_buff,"%c%c%08d",&info_dat_type[0],&info_dat_type[1],&info_dat_len)!=3){
				break;
			}
			printf("received frame type %s with length %d\n",(char*)info_dat_type,info_dat_len);
			if(info_dat_len>50000){
				break;
			}
			received_bytes = SSL_read(sd->ssl, sd->receiver_buffer, info_dat_len); 
			if(received_bytes!=info_dat_len){
				break;
			}
			sd->av_pkt.size=info_dat_len;  
			sd->av_pkt.data = sd->receiver_buffer;
			if (info_dat_type[0]=='v' && info_dat_type[1]=='f'){//video frame
				int got_frame;
				avcodec_decode_video_notdeprecated(sd->av_ctx, sd->av_frame, &got_frame, &sd->av_pkt);
				if (!got_frame) {
					fprintf(stderr, "Error while decoding frame \n");
				}else {
					printf("pdata=%p format=%d linesize=%d width=%d height=%d\n",
						sd->av_frame->data[0],
						(AVPixelFormat)sd->av_frame->format,
						sd->av_frame->linesize[0],
						sd->av_frame->width,
						sd->av_frame->height);
					if(sd->img_convert_ctx == NULL) 
					{
						sd->img_convert_ctx = sws_getContext(
							sd->av_frame->width,
							sd->av_frame->height,
							(AVPixelFormat)sd->av_frame->format,
							VIDEO_WIDTH,
							VIDEO_HEIGHT,
							AV_PIX_FMT_RGB32,
							SWS_BICUBIC,
							NULL,
							NULL,
							NULL);
						if(sd->img_convert_ctx == NULL) {
							fprintf(stderr, "Cannot initialize the conversion context!\n");
							free_servlet_data(sd);
							return NULL;
						}//init error (sd->img_convert_ctx SECOND(!!))
					}//img_convert_ctx not null
					sws_scale(
					sd->img_convert_ctx,
					(const uint8_t**)sd->av_frame->data, 
					sd->av_frame->linesize,
					0, 
					sd->av_frame->height, 
					sd->av_scaled_frame->data,
					sd->av_scaled_frame->linesize );
					memcpy(received_picture_rgb32,sd->av_scaled_frame->data[0],VIDEO_WIDTH*VIDEO_HEIGHT*4);
				}//else if (everythink ok and len > 0 and got_frame
			}//if video frame (vf)
		}//for
	}//else
	free_servlet_data(sd);
	return NULL;
}

//

long frames_would_be_send;

pthread_mutex_t senderClckThread_mutex;
void* senderClckThread(void* p){
	frames_would_be_send=0;
	for(;;){
		pthread_mutex_lock(&senderClckThread_mutex);
		frames_would_be_send++;
		pthread_mutex_unlock(&senderClckThread_mutex);
		usleep(1000000/24);// 24fps		
	}
	return NULL;
}

void* senderThread(void* p){
	int sd;
	while( ( sd=create_client_and_connect(PORT_CAM_OUT,1))!=-1 ){
			frames_would_be_send=0;
			printf("connected\n");
			for(;;){
				pthread_mutex_lock(&senderClckThread_mutex);
				if(frames_would_be_send>0){
					frames_would_be_send--;
					pthread_mutex_unlock(&senderClckThread_mutex);
//					printf("frames_would_be_send=%d\n",frames_would_be_send);
					if(! send_all(sd,received_picture_rgb32,VIDEO_WIDTH*VIDEO_HEIGHT*4) ){perror("send err");break;}
				}else{
					pthread_mutex_unlock(&senderClckThread_mutex);
					usleep(10000);			
				}
			}//for;;
		close(sd);
	}//main loop
	return NULL;
}

int main()
{ 
	signal(SIGPIPE,SIG_IGN);
	int server;
	avcodec_register_all();
	
	received_picture_rgb32=(uint8_t*)malloc(VIDEO_WIDTH*VIDEO_HEIGHT*4);
	if(received_picture_rgb32==NULL){
		perror("cannot alloc memory for received_picture_rgb32");
		exit(0);
	}

	const char* CertFile = CAM_SSL_SERVER_CERT_PERM ;
	const char* KeyFile = CAM_SSL_SERVER_KEY_PEM;

	SSL_library_init();

	pthread_t senderThread_t;
	pthread_t senderClckThread_t;

	pthread_create (&senderThread_t,NULL,senderThread,NULL);
	pthread_create (&senderClckThread_t,NULL,senderClckThread,NULL);

	ssl_ctx = InitServerCTX();
	LoadCertificates(ssl_ctx, CertFile, KeyFile);
	server = create_listener(PORT_CAM_IN,"0.0.0.0");
	for (;;) {
		struct sockaddr_in addr;
		socklen_t len = sizeof(addr);

		int client = accept(server, (struct sockaddr*)&addr, &len);
		printf("Connection: %s:%d\n",inet_ntoa(addr.sin_addr), ntohs(addr.sin_port));
		
		servlet_data_t * servlet_data=create_servlet_data(client);
		pthread_create(&servlet_data->thread,NULL,Servlet,(void*)servlet_data);
	}
	close(server);
	SSL_CTX_free(ssl_ctx);
	free(received_picture_rgb32);
}
