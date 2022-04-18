/* ---------------------------------------------------------------------------
** This software is in the public domain, furnished "as is", without technical
** support, and with no warranty, express or implied, as to its usefulness for
** any purpose.
**
** rtmpvideosource.h
**
** -------------------------------------------------------------------------*/

#pragma once

#include <string.h>
#include <vector>
#include <queue>
#include <thread>
#include <mutex>
#include <condition_variable>

#include "media/base/codec.h"
#include "media/base/video_common.h"
#include "media/base/video_broadcaster.h"
#include "media/engine/internal_decoder_factory.h"

#include "common_video/h264/h264_common.h"
#include "common_video/h264/sps_parser.h"
#include "modules/video_coding/h264_sprop_parameter_sets.h"

#include "api/video_codecs/video_decoder.h"

#include "VideoDecoder.h"

#include <librtmp/rtmp.h>
#include <librtmp/log.h>

class RtmpVideoSource : public rtc::VideoSourceInterface<webrtc::VideoFrame>
{
public:
    static RtmpVideoSource *Create(const std::string &url, const std::map<std::string, std::string> &opts, std::unique_ptr<webrtc::VideoDecoderFactory> &videoDecoderFactory)
    {
        std::unique_ptr<RtmpVideoSource> capturer(new RtmpVideoSource(url, opts, videoDecoderFactory));
        return capturer.release();
    }
    virtual ~RtmpVideoSource()
    {
        this->Stop();
        RTMP_Close(&m_rtmp);
    }

private:
    RtmpVideoSource(const std::string &uri, const std::map<std::string, std::string> &opts, std::unique_ptr<webrtc::VideoDecoderFactory> &videoDecoderFactory) : 
        m_stop(false),
        m_url(uri),
        m_decoder(m_broadcaster, opts, videoDecoderFactory)
    {
        RTMP_Init(&m_rtmp);
        RTMP_LogSetOutput(stderr);
        RTMP_LogSetLevel(RTMP_LOGINFO);
        if (!RTMP_SetupURL(&m_rtmp, const_cast<char*>(m_url.c_str()))) {
            RTC_LOG(LS_INFO) << "Unable to parse rtmp url:" << m_url;
        }

        this->Start();
    }

    void Start()
    {
        RTC_LOG(LS_INFO) << "RtmpVideoSource::Start";
        m_capturethread = std::thread(&RtmpVideoSource::CaptureThread, this);
        m_decoder.Start();
    }
    void Stop()
    {
        RTC_LOG(LS_INFO) << "RtmpVideoSource::stop";
        m_stop = true;
        m_capturethread.join();
        m_decoder.Stop();
    }
    bool IsRunning() { return (!m_stop); }

    void CaptureThread()
    {
        RTC_LOG(LS_INFO) << "RtmpVideoSource::CaptureThread begin";
        while (!m_stop)
        {

            if ( !RTMP_IsConnected(&m_rtmp) && (!RTMP_Connect(&m_rtmp, NULL) || !RTMP_ConnectStream(&m_rtmp, 0)) ) {
                RTC_LOG(LS_INFO) << "Unable to connect to stream";
            } 

            if (RTMP_IsConnected(&m_rtmp)) {
                if (RTMP_ReadPacket(&m_rtmp, &m_packet)) {
                    int64_t ts = RTMP_GetTime();

                    RTMPPacket_Dump(&m_packet);

                    if (m_packet.m_packetType == RTMP_PACKET_TYPE_VIDEO) {
                        if (m_packet.m_body[0] == 0x17 && m_packet.m_body[1] == 0) {
                            RTC_LOG(LS_INFO) << "RtmpVideoSource::onNewSession SPS/PPS";
                            webrtc::H264::NaluType nalu_type = webrtc::H264::ParseNaluType(m_packet.m_body[13]);
                            RTC_LOG(LS_INFO) << "RtmpVideoSource::onNewSession NALU type:" << nalu_type;
                            if (nalu_type == webrtc::H264::NaluType::kSps)
                            {
                                m_cfg.clear();
                                int spssize = (m_packet.m_body[11]<<8) + m_packet.m_body[12];
                                RTC_LOG(LS_INFO) << "RtmpVideoSource::onNewSession SPS size:" << spssize;
                                absl::optional<webrtc::SpsParser::SpsState> sps = webrtc::SpsParser::ParseSps((const unsigned char*)(&m_packet.m_body[14]), spssize);
                                if (!sps)
                                {
                                    RTC_LOG(LS_ERROR) << "cannot parse sps";
                                } else {
                                    RTC_LOG(LS_ERROR) << "sps " << sps->width << "x" << sps->height;
                                    this->resetDecoderWhenGeometryUpdated(sps);
				    m_cfg.insert(m_cfg.end(), H26X_marker, H26X_marker+sizeof(H26X_marker));
                                    m_cfg.insert(m_cfg.end(), &m_packet.m_body[13], &m_packet.m_body[13] + spssize);

                                    nalu_type = webrtc::H264::ParseNaluType(m_packet.m_body[16+spssize]);
                                    RTC_LOG(LS_INFO) << "RtmpVideoSource::onNewSession NALU type:" << nalu_type;
                                    int ppssize = (m_packet.m_body[14+spssize]<<8) + m_packet.m_body[15+spssize];
                                    RTC_LOG(LS_INFO) << "RtmpVideoSource::onNewSession PPS size:" << ppssize;

				    m_cfg.insert(m_cfg.end(), H26X_marker, H26X_marker+sizeof(H26X_marker));
                                    m_cfg.insert(m_cfg.end(), &m_packet.m_body[16+spssize], &m_packet.m_body[16+spssize] + ppssize);
                                }                                
                            } 
                        } else if (m_packet.m_body[0] == 0x17 && m_packet.m_body[1] == 1) {
                            int framesize = ((((unsigned char)m_packet.m_body[7])&0xff)<<8) + m_packet.m_body[8];
                            RTC_LOG(LS_INFO) << "RtmpVideoSource::onNewSession IDR size:" << framesize;                            
                            webrtc::H264::NaluType nalu_type = webrtc::H264::ParseNaluType(m_packet.m_body[9]);
                            RTC_LOG(LS_INFO) << "RtmpVideoSource::onNewSession IDR type:" << nalu_type;

                            std::vector<uint8_t> content;
                            content.insert(content.end(), m_cfg.begin(), m_cfg.end());
			    content.insert(content.end(), H26X_marker, H26X_marker+sizeof(H26X_marker));
                            content.insert(content.end(), &m_packet.m_body[9], &m_packet.m_body[9]+framesize);
                            rtc::scoped_refptr<webrtc::EncodedImageBuffer> frame = webrtc::EncodedImageBuffer::Create(content.data(), content.size());
                            m_decoder.PostFrame(frame, ts, webrtc::VideoFrameType::kVideoFrameKey);

                        }
                        else if (m_packet.m_body[0] == 0x27) {
                            int framesize = ((((unsigned char)m_packet.m_body[7])&0xff)<<8) + m_packet.m_body[8];
                            RTC_LOG(LS_INFO) << "RtmpVideoSource::onNewSession Slice size:" << framesize;                            
                            webrtc::H264::NaluType nalu_type = webrtc::H264::ParseNaluType(m_packet.m_body[9]);
                            RTC_LOG(LS_INFO) << "RtmpVideoSource::onNewSession Slice NALU type:" << nalu_type;                            
                            std::vector<uint8_t> content;
			    content.insert(content.end(), H26X_marker, H26X_marker+sizeof(H26X_marker));
                            content.insert(content.end(), &m_packet.m_body[9], &m_packet.m_body[9]+framesize);
                            rtc::scoped_refptr<webrtc::EncodedImageBuffer> frame = webrtc::EncodedImageBuffer::Create(content.data(), content.size());
                            m_decoder.PostFrame(frame, ts, webrtc::VideoFrameType::kVideoFrameDelta);
                        }
                    }
                }
                RTMPPacket_Free(&m_packet);
            }
        }
        RTC_LOG(LS_INFO) << "RtmpVideoSource::CaptureThread end";
    }

    void resetDecoderWhenGeometryUpdated(const absl::optional<webrtc::SpsParser::SpsState>& sps) {
        if (m_decoder.hasDecoder())
        {
            if ((m_format.width != sps->width) || (m_format.height != sps->height))
            {
                RTC_LOG(LS_INFO) << "format changed => set format from " << m_format.width << "x" << m_format.height << " to " << sps->width << "x" << sps->height;
                m_decoder.destroyDecoder();
            }
        }

        if (!m_decoder.hasDecoder())
        {
            int fps = 25;
            RTC_LOG(LS_INFO) << "RtmpVideoSource:onData SPS set format " << sps->width << "x" << sps->height << " fps:" << fps;
            cricket::VideoFormat videoFormat(sps->width, sps->height, cricket::VideoFormat::FpsToInterval(fps), cricket::FOURCC_I420);
            m_format = videoFormat;

            m_decoder.createDecoder("H264", sps->width, sps->height);
        }
    }

    // overide rtc::VideoSourceInterface<webrtc::VideoFrame>
    void AddOrUpdateSink(rtc::VideoSinkInterface<webrtc::VideoFrame> *sink, const rtc::VideoSinkWants &wants)
    {
        m_broadcaster.AddOrUpdateSink(sink, wants);
    }

    void RemoveSink(rtc::VideoSinkInterface<webrtc::VideoFrame> *sink)
    {
        m_broadcaster.RemoveSink(sink);
    }

private:
    char m_stop;

protected:
    RTMP m_rtmp;
    RTMPPacket m_packet;
    std::string m_url;

private:
    std::thread m_capturethread;
    cricket::VideoFormat m_format;
    std::vector<uint8_t> m_cfg;

    rtc::VideoBroadcaster m_broadcaster;
    VideoDecoder m_decoder;
};
