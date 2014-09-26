/*
 * Copyright 2014 Intel Corporation All Rights Reserved. 
 * 
 * The source code contained or described herein and all documents related to the 
 * source code ("Material") are owned by Intel Corporation or its suppliers or 
 * licensors. Title to the Material remains with Intel Corporation or its suppliers 
 * and licensors. The Material contains trade secrets and proprietary and 
 * confidential information of Intel or its suppliers and licensors. The Material 
 * is protected by worldwide copyright and trade secret laws and treaty provisions. 
 * No part of the Material may be used, copied, reproduced, modified, published, 
 * uploaded, posted, transmitted, distributed, or disclosed in any way without 
 * Intel's prior express written permission.
 * 
 * No license under any patent, copyright, trade secret or other intellectual 
 * property right is granted to or conferred upon you by disclosure or delivery of 
 * the Materials, either expressly, by implication, inducement, estoppel or 
 * otherwise. Any license under such intellectual property rights must be express 
 * and approved by Intel in writing.
 */

#include "MCUMixer.h"

#include "BufferManager.h"
#include "VCMMediaProcessor.h"
#include "ACMMediaProcessor.h"
#include <ProtectedRTPReceiver.h>
#include <ProtectedRTPSender.h>
#include <WoogeenTransport.h>

using namespace webrtc;
using namespace woogeen_base;
using namespace erizo;

namespace mcu {

DEFINE_LOGGER(MCUMixer, "mcu.MCUMixer");

MCUMixer::MCUMixer()
{
    this->init();
}

MCUMixer::~MCUMixer()
{
    closeAll();
}

/**
 * init could be used for reset the state of this MCUMixer
 */
bool MCUMixer::init()
{
    feedback_.reset(new DummyFeedbackSink());
    bufferManager_.reset(new BufferManager());
    videoTransport_.reset(new WoogeenVideoTransport(this));
    vop_.reset(new VCMOutputProcessor());
    vop_->init(videoTransport_.get(), bufferManager_.get());

    audioTransport_.reset(new WoogeenAudioTransport(this));
    aop_.reset(new ACMOutputProcessor(1, audioTransport_.get()));

    return true;

  }

  int MCUMixer::deliverAudioData(char* buf, int len, MediaSource* from) 
{
    std::map<erizo::MediaSource*, boost::shared_ptr<woogeen_base::ProtectedRTPReceiver>>::iterator it = publishers_.find(from);
    if (it != publishers_.end() && it->second)
        return it->second->deliverAudioData(buf, len);

    return 0;
  }

/**
 * use vcm to decode/compose/encode the streams, and then deliver to all subscribers
 * multiple publishers may call to this method simultaneously from different threads.
 * the incoming buffer is a rtp packet
 */
int MCUMixer::deliverVideoData(char* buf, int len, MediaSource* from)
{
    std::map<erizo::MediaSource*, boost::shared_ptr<woogeen_base::ProtectedRTPReceiver>>::iterator it = publishers_.find(from);
    if (it != publishers_.end() && it->second)
        return it->second->deliverVideoData(buf, len);

    return 0;
}

void MCUMixer::receiveRtpData(char* buf, int len, erizo::DataType type, uint32_t streamId)
{
    if (subscribers_.empty() || len <= 0)
        return;

    std::map<std::string, boost::shared_ptr<MediaSink>>::iterator it;
    boost::unique_lock<boost::mutex> lock(myMonitor_);
    switch (type) {
    case erizo::AUDIO: {
        for (it = subscribers_.begin(); it != subscribers_.end(); ++it) {
            if ((*it).second != NULL)
                (*it).second->deliverAudioData(buf, len);
        }
        break;
    }
    case erizo::VIDEO: {
        for (it = subscribers_.begin(); it != subscribers_.end(); ++it) {
            if ((*it).second != NULL)
                (*it).second->deliverVideoData(buf, len);
        }
        break;
    }
    default:
        break;
    }
}

/**
 * Attach a new InputStream to the Transcoder
 */
void MCUMixer::addPublisher(MediaSource* puber)
{
    int index = assignSlot(puber);
    ELOG_DEBUG("addPublisher - assigned slot is %d", index);
    std::map<erizo::MediaSource*, boost::shared_ptr<woogeen_base::ProtectedRTPReceiver>>::iterator it = publishers_.find(puber);
    if (it == publishers_.end() || !it->second) {
        vop_->updateMaxSlot(maxSlot());
        boost::shared_ptr<VCMInputProcessor> ip(new VCMInputProcessor(index, vop_.get()));
        ip->init(bufferManager_.get());
	ACMInputProcessor* aip = new ACMInputProcessor(index);
	aip->Init(aop_.get());
	ip->setAudioInputProcessor(aip);
        publishers_[puber].reset(new ProtectedRTPReceiver(ip));
        //add to audio mixer
        aop_->SetMixabilityStatus(*aip, true);
    } else {
        assert("new publisher added with InputProcessor still available");    // should not go there
    }
}

void MCUMixer::addSubscriber(MediaSink* suber, const std::string& peerId)
{
    ELOG_DEBUG("Adding subscriber: videoSinkSSRC is %d", suber->getVideoSinkSSRC());
    boost::mutex::scoped_lock lock(myMonitor_);
    FeedbackSource* fbsource = suber->getFeedbackSource();

    if (fbsource!=NULL){
      ELOG_DEBUG("adding fbsource");
      fbsource->setFeedbackSink(feedback_.get());
    }
    subscribers_[peerId] = boost::shared_ptr<MediaSink>(suber);
}

void MCUMixer::removeSubscriber(const std::string& peerId)
{
    ELOG_DEBUG("removing subscriber: peerId is %s",   peerId.c_str());
    boost::mutex::scoped_lock lock(myMonitor_);
    if (subscribers_.find(peerId) != subscribers_.end()) {
      subscribers_.erase(peerId);
    }
}

void MCUMixer::removePublisher(MediaSource* puber)
{
    std::map<erizo::MediaSource*, boost::shared_ptr<woogeen_base::ProtectedRTPReceiver>>::iterator it = publishers_.find(puber);
    if (it != publishers_.end()) {
        int index = getSlot(puber);
        assert(index >= 0);
        puberSlotMap_[index] = NULL;
        publishers_.erase(it);
    }
}

void MCUMixer::closeAll()
{
    boost::unique_lock<boost::mutex> lock(myMonitor_);
    ELOG_DEBUG ("Mixer closeAll");
    std::map<std::string, boost::shared_ptr<MediaSink>>::iterator it = subscribers_.begin();
    while (it != subscribers_.end()) {
      if ((*it).second != NULL) {
        FeedbackSource* fbsource = (*it).second->getFeedbackSource();
        if (fbsource!=NULL){
          fbsource->setFeedbackSink(NULL);
        }
      }
      subscribers_.erase(it++);
    }
    lock.unlock();
    lock.lock();
    subscribers_.clear();
    // TODO: publishers
    ELOG_DEBUG ("ClosedAll media in this Mixer");

}

}/* namespace mcu */

