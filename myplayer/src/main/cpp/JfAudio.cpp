//
// Created by zjf on 2019/1/4.
//

#include "JfAudio.h"

JfAudio::JfAudio(JfPlayStatus *playStatus,int sample_rate,JfCallJava *callJava) {
    this->playStatus = playStatus;
    this->sample_rate = sample_rate;
    this->callJava = callJava;

    queue = new JfQueue(playStatus);
    buffer = (uint8_t *)(av_malloc(sample_rate * 2 * 2));//每秒的pcm数据
}

JfAudio::~JfAudio() {

}


void *decodePlay(void *data){
    JfAudio *jfAudio = (JfAudio *)(data);
    //jfAudio->resampleAudio();
    jfAudio->initOpenSLES();
    pthread_exit(&jfAudio->playThread);
}
void JfAudio::play() {
    pthread_create(&playThread,NULL,decodePlay,this);
}


int JfAudio::resampleAudio() {
    while (playStatus != NULL && !playStatus->exit){

        if (queue->getQueueSize() == 0){//加载状态
            if (!playStatus->loading){
                playStatus->loading = true;
                callJava->onCallLoading(CHILD_THREAD,playStatus->loading);
            }
            continue;
        } else {//播放状态
            if (playStatus->loading){
                playStatus->loading = false;
                callJava->onCallLoading(CHILD_THREAD,playStatus->loading);
            }
        }


        avPacket = av_packet_alloc();
        if (queue->getAVPacket(avPacket) != 0){
            av_packet_free(&avPacket);
            av_free(avPacket);
            avPacket = NULL;
            continue;
        }
        ret = avcodec_send_packet(pACodecCtx,avPacket);
        if (ret != NULL){
            av_packet_free(&avPacket);
            av_free(avPacket);
            avPacket = NULL;
            continue;
        }

        avFrame = av_frame_alloc();
        ret = avcodec_receive_frame(pACodecCtx,avFrame);
        if (ret == 0) {//进行重采样
            if (avFrame->channels > 0 && avFrame->channel_layout == 0){//有声道数没有声道布局，所以要设置声道布局
                avFrame->channel_layout = av_get_default_channel_layout(avFrame->channels);//设置声道布局
            } else if (avFrame->channels == 0 && avFrame->channel_layout > 0){//有声道布局没有声道数，所以要设置声道数
                avFrame->channels = av_get_channel_layout_nb_channels(avFrame->channel_layout);
            }

            SwrContext *swr_ctx = NULL;
            swr_ctx = swr_alloc_set_opts(NULL,
                    AV_CH_LAYOUT_STEREO,// 输出声道布局:立体声
                    AV_SAMPLE_FMT_S16,//输出采样位数格式
                    avFrame->sample_rate,//输出的采样率
                    avFrame->channel_layout,//输入声道布局
                    (AVSampleFormat)(avFrame->format),//输入采样位数格式
                    avFrame->sample_rate,//输入采样率
                    NULL,
                    NULL);

            if (!swr_ctx || swr_init(swr_ctx) < 0){

                av_packet_free(&avPacket);
                av_free(avPacket);
                avPacket = NULL;

                av_frame_free(&avFrame);
                av_free(avFrame);
                avFrame = NULL;

                if (swr_ctx != NULL){
                    swr_free(&swr_ctx);
                    swr_ctx = NULL;
                }
                if (LOG_DEBUG){
                    LOGE("!swr_ctx || swr_init(swr_ctx) < 0");
                }
                continue;
            }

            //返回的是采样的个数
            int nb = swr_convert(swr_ctx,
                                &buffer,//转码后输出的PCM数据大小
                                avFrame->nb_samples,//输出采样个数
                                (const uint8_t **)(avFrame->data),
                                avFrame->nb_samples);

            int out_channels = av_get_channel_layout_nb_channels(AV_CH_LAYOUT_STEREO);

            data_size = nb * out_channels * av_get_bytes_per_sample(AV_SAMPLE_FMT_S16);

            now_time = avFrame->pts * av_q2d(time_base);

            if (now_time < clock){
                now_time = clock;
            }
            clock = now_time;
            /*if (LOG_DEBUG){
                LOGD("DATA SIZE == %d",data_size);
            }*/

            av_packet_free(&avPacket);
            av_free(avPacket);
            avPacket = NULL;

            av_frame_free(&avFrame);
            av_free(avFrame);
            avFrame = NULL;

            swr_free(&swr_ctx);
            swr_ctx = NULL;
            break;
        } else {
            av_packet_free(&avPacket);
            av_free(avPacket);
            avPacket = NULL;

            av_frame_free(&avFrame);
            av_free(avFrame);
            avFrame = NULL;
            continue;
        }
    }
    return data_size;
}


void pcmBufferCallback(SLAndroidSimpleBufferQueueItf bf,void *context){
    //获取pcm数据
    JfAudio *jfAudio = (JfAudio *)context;
    if (jfAudio != NULL){
        int buffer_size = jfAudio->resampleAudio();
        if (buffer_size > 0){
            jfAudio->clock += buffer_size / ((double)(jfAudio->sample_rate * 2 * 2));

            if (jfAudio->clock - jfAudio->last_time > 0.1){//每秒回调10次就够了
                jfAudio->last_time = jfAudio->clock;
                jfAudio->callJava->onCallTimeInfo(CHILD_THREAD,jfAudio->clock,jfAudio->duration);
            }

            (*jfAudio->pcmBufferQueue)->Enqueue(jfAudio->pcmBufferQueue,jfAudio->buffer,buffer_size);
        }
    } else {
        if (LOG_DEBUG){
            LOGE("jfAudio == NULL");
        }
    }
}

void JfAudio::initOpenSLES() {
    // 1、创建一个引擎：三部曲 slCreateEngine->Realize->GetInterface
    slCreateEngine(&engineObject,0,0,0,0,0);
    (*engineObject)->Realize(engineObject,SL_BOOLEAN_FALSE);
    (*engineObject)->GetInterface(engineObject,SL_IID_ENGINE,&engineEngine);


    // 2、设置混音器：创建混音器引擎->设置混音器属性
    const SLInterfaceID mids[1] = {SL_IID_ENVIRONMENTALREVERB};
    const SLboolean mrep[1] = {SL_BOOLEAN_FALSE};

    (*engineEngine)->CreateOutputMix(engineEngine,&outputMixObject,1,mids,mrep);
    (*outputMixObject)->Realize(outputMixObject,SL_BOOLEAN_FALSE);
    (*outputMixObject)->GetInterface(outputMixObject,SL_IID_ENVIRONMENTALREVERB,&outputMixEnvReb);

    (*outputMixEnvReb)->SetEnvironmentalReverbProperties(outputMixEnvReb,&reverbSettings);


    // 3、创建播放器
    SLDataLocator_AndroidSimpleBufferQueue android_queue = {SL_DATALOCATOR_ANDROIDSIMPLEBUFFERQUEUE,2};//一个队列
    SLDataFormat_PCM format_pcm = {//设置PCM播放时的属性
            SL_DATAFORMAT_PCM,
            2,
            getCurrentSampleRateForOpenSLES(sample_rate),
            SL_PCMSAMPLEFORMAT_FIXED_16,
            SL_PCMSAMPLEFORMAT_FIXED_16,
            SL_SPEAKER_FRONT_LEFT | SL_SPEAKER_FRONT_RIGHT,
            SL_BYTEORDER_LITTLEENDIAN
    };
    SLDataSource slDataSource ={&android_queue,&format_pcm};

    SLDataLocator_OutputMix outputMix = {SL_DATALOCATOR_OUTPUTMIX,outputMixObject};
    SLDataSink audioSink = {&outputMix,NULL};

    const SLInterfaceID ids[1] = {SL_IID_BUFFERQUEUE};
    const SLboolean req[1] = {SL_BOOLEAN_TRUE};
    (*engineEngine)->CreateAudioPlayer(engineEngine,&pcmPlayerObject,&slDataSource,&audioSink,1,ids,req);
    (*pcmPlayerObject)->Realize(pcmPlayerObject,SL_BOOLEAN_FALSE);
    (*pcmPlayerObject)->GetInterface(pcmPlayerObject,SL_IID_PLAY,&pcmPlayerPlay);


    //4、设置缓冲队列和回调函数
    (*pcmPlayerObject)->GetInterface(pcmPlayerObject,SL_IID_BUFFERQUEUE,&pcmBufferQueue);
    (*pcmBufferQueue)->RegisterCallback(pcmBufferQueue,pcmBufferCallback,this);


    //5、设置播放状态
    (*pcmPlayerPlay)->SetPlayState(pcmPlayerPlay,SL_PLAYSTATE_PLAYING);

    pcmBufferCallback(pcmBufferQueue,this);
}

uint JfAudio::getCurrentSampleRateForOpenSLES(int sample_rate) {
    int rate = 0;
    switch (sample_rate){
        case 8000:
            rate = SL_SAMPLINGRATE_8;
            break;
        case 11025:
            rate = SL_SAMPLINGRATE_11_025;
            break;
        case 12000:
            rate = SL_SAMPLINGRATE_12;
            break;
        case 16000:
            rate = SL_SAMPLINGRATE_16;
            break;
        case 22050:
            rate = SL_SAMPLINGRATE_22_05;
            break;
        case 24000:
            rate = SL_SAMPLINGRATE_24;
            break;
        case 32000:
            rate = SL_SAMPLINGRATE_32;
            break;
        case 44100:
            rate = SL_SAMPLINGRATE_44_1;
            break;
        case 48000:
            rate = SL_SAMPLINGRATE_48;
            break;
        case 64000:
            rate = SL_SAMPLINGRATE_64;
            break;
        case 88200:
            rate = SL_SAMPLINGRATE_88_2;
            break;
        case 96000:
            rate = SL_SAMPLINGRATE_96;
            break;
        case 192000:
            rate = SL_SAMPLINGRATE_192;
            break;
        default:
            rate =  SL_SAMPLINGRATE_44_1;
    }
    return rate;
}

void JfAudio::pause() {
    if (pcmPlayerObject != NULL){
        (*pcmPlayerPlay)->SetPlayState(pcmPlayerPlay,  SL_PLAYSTATE_PAUSED);
    }
}

void JfAudio::resume() {
    if (pcmPlayerObject != NULL){
        (*pcmPlayerPlay)->SetPlayState(pcmPlayerPlay,  SL_PLAYSTATE_PLAYING);
    }
}
