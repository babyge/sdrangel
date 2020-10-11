///////////////////////////////////////////////////////////////////////////////////
// Copyright (C) 2017 Edouard Griffiths, F4EXB                                   //
// Copyright (C) 2020 Jon Beniston, M7RCE                                        //
//                                                                               //
// This program is free software; you can redistribute it and/or modify          //
// it under the terms of the GNU General Public License as published by          //
// the Free Software Foundation as version 3 of the License, or                  //
// (at your option) any later version.                                           //
//                                                                               //
// This program is distributed in the hope that it will be useful,               //
// but WITHOUT ANY WARRANTY; without even the implied warranty of                //
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the                  //
// GNU General Public License V3 for more details.                               //
//                                                                               //
// You should have received a copy of the GNU General Public License             //
// along with this program. If not, see <http://www.gnu.org/licenses/>.          //
///////////////////////////////////////////////////////////////////////////////////

#include <cstddef>
#include <string.h>

#include <QMutexLocker>
#include <QDebug>
#include <QNetworkReply>
#include <QBuffer>

#include <uhd/usrp/multi_usrp.hpp>

#include "SWGDeviceSettings.h"
#include "SWGUSRPOutputSettings.h"
#include "SWGDeviceState.h"
#include "SWGDeviceReport.h"
#include "SWGUSRPOutputReport.h"

#include "device/deviceapi.h"
#include "dsp/dspcommands.h"
#include "dsp/dspengine.h"
#include "usrpoutputthread.h"
#include "usrp/deviceusrpparam.h"
#include "usrp/deviceusrp.h"
#include "usrpoutput.h"

MESSAGE_CLASS_DEFINITION(USRPOutput::MsgConfigureUSRP, Message)
MESSAGE_CLASS_DEFINITION(USRPOutput::MsgStartStop, Message)
MESSAGE_CLASS_DEFINITION(USRPOutput::MsgGetStreamInfo, Message)
MESSAGE_CLASS_DEFINITION(USRPOutput::MsgGetDeviceInfo, Message)
MESSAGE_CLASS_DEFINITION(USRPOutput::MsgReportStreamInfo, Message)


USRPOutput::USRPOutput(DeviceAPI *deviceAPI) :
    m_deviceAPI(deviceAPI),
    m_settings(),
    m_usrpOutputThread(nullptr),
    m_deviceDescription("USRPOutput"),
    m_running(false),
    m_channelAcquired(false)
{
    m_deviceAPI->setNbSinkStreams(1);
    m_sampleSourceFifo.resize(SampleSourceFifo::getSizePolicy(m_settings.m_devSampleRate));
    m_streamId = nullptr;
    suspendRxBuddies();
    suspendTxBuddies();
    openDevice();
    resumeTxBuddies();
    resumeRxBuddies();
    m_networkManager = new QNetworkAccessManager();
    connect(m_networkManager, SIGNAL(finished(QNetworkReply*)), this, SLOT(networkManagerFinished(QNetworkReply*)));
}

USRPOutput::~USRPOutput()
{
    disconnect(m_networkManager, SIGNAL(finished(QNetworkReply*)), this, SLOT(networkManagerFinished(QNetworkReply*)));
    delete m_networkManager;

    if (m_running) {
        stop();
    }

    suspendRxBuddies();
    suspendTxBuddies();
    closeDevice();
    resumeTxBuddies();
    resumeRxBuddies();
}

void USRPOutput::destroy()
{
    delete this;
}

bool USRPOutput::openDevice()
{
    int requestedChannel = m_deviceAPI->getDeviceItemIndex();

    // look for Tx buddies and get reference to common parameters
    // if there is a channel left take the first available
    if (m_deviceAPI->getSinkBuddies().size() > 0) // look sink sibling first
    {
        qDebug("USRPOutput::openDevice: look in Ix buddies");

        DeviceAPI *sinkBuddy = m_deviceAPI->getSinkBuddies()[0];
        //m_deviceShared = *((DeviceUSRPShared *) sinkBuddy->getBuddySharedPtr()); // copy shared data
        DeviceUSRPShared *deviceUSRPShared = (DeviceUSRPShared*) sinkBuddy->getBuddySharedPtr();
        m_deviceShared.m_deviceParams = deviceUSRPShared->m_deviceParams;

        DeviceUSRPParams *deviceParams = m_deviceShared.m_deviceParams; // get device parameters

        if (deviceParams == 0)
        {
            qCritical("USRPOutput::openDevice: cannot get device parameters from Tx buddy");
            return false; // the device params should have been created by the buddy
        }
        else
        {
            qDebug("USRPOutput::openDevice: getting device parameters from Tx buddy");
        }

        if (m_deviceAPI->getSinkBuddies().size() == deviceParams->m_nbTxChannels)
        {
            qCritical("USRPOutput::openDevice: no more Tx channels available in device");
            return false; // no more Tx channels available in device
        }
        else
        {
            qDebug("USRPOutput::openDevice: at least one more Tx channel is available in device");
        }

        // check if the requested channel is busy and abort if so (should not happen if device management is working correctly)

        for (unsigned int i = 0; i < m_deviceAPI->getSinkBuddies().size(); i++)
        {
            DeviceAPI *buddy = m_deviceAPI->getSinkBuddies()[i];
            DeviceUSRPShared *buddyShared = (DeviceUSRPShared *) buddy->getBuddySharedPtr();

            if (buddyShared->m_channel == requestedChannel)
            {
                qCritical("USRPOutput::openDevice: cannot open busy channel %u", requestedChannel);
                return false;
            }
        }

        m_deviceShared.m_channel = requestedChannel; // acknowledge the requested channel
    }
    // look for Rx buddies and get reference to common parameters
    // take the first Rx channel
    else if (m_deviceAPI->getSourceBuddies().size() > 0) // then source
    {
        qDebug("USRPOutput::openDevice: look in Rx buddies");

        DeviceAPI *sourceBuddy = m_deviceAPI->getSourceBuddies()[0];
        //m_deviceShared = *((DeviceUSRPShared *) sourceBuddy->getBuddySharedPtr()); // copy parameters
        DeviceUSRPShared *deviceUSRPShared = (DeviceUSRPShared*) sourceBuddy->getBuddySharedPtr();
        m_deviceShared.m_deviceParams = deviceUSRPShared->m_deviceParams;

        if (m_deviceShared.m_deviceParams == 0)
        {
            qCritical("USRPOutput::openDevice: cannot get device parameters from Rx buddy");
            return false; // the device params should have been created by the buddy
        }
        else
        {
            qDebug("USRPOutput::openDevice: getting device parameters from Rx buddy");
        }

        m_deviceShared.m_channel = requestedChannel; // acknowledge the requested channel
    }
    // There are no buddies then create the first USRP common parameters
    // open the device this will also populate common fields
    // take the first Tx channel
    else
    {
        qDebug("USRPOutput::openDevice: open device here");

        m_deviceShared.m_deviceParams = new DeviceUSRPParams();
        char serial[256];
        strcpy(serial, qPrintable(m_deviceAPI->getSamplingDeviceSerial()));
        m_deviceShared.m_deviceParams->open(serial);
        m_deviceShared.m_channel = requestedChannel; // acknowledge the requested channel
    }

    m_deviceAPI->setBuddySharedPtr(&m_deviceShared); // propagate common parameters to API

    return true;
}

void USRPOutput::suspendRxBuddies()
{
    const std::vector<DeviceAPI*>& sourceBuddies = m_deviceAPI->getSourceBuddies();
    std::vector<DeviceAPI*>::const_iterator itSource = sourceBuddies.begin();

    qDebug("USRPOutput::suspendRxBuddies (%lu)", sourceBuddies.size());

    for (; itSource != sourceBuddies.end(); ++itSource)
    {
        DeviceUSRPShared *buddySharedPtr = (DeviceUSRPShared *) (*itSource)->getBuddySharedPtr();

        if (buddySharedPtr->m_thread && buddySharedPtr->m_thread->isRunning())
        {
            buddySharedPtr->m_thread->stopWork();
            buddySharedPtr->m_threadWasRunning = true;
        }
        else
        {
            buddySharedPtr->m_threadWasRunning = false;
        }
    }
}

void USRPOutput::suspendTxBuddies()
{
    const std::vector<DeviceAPI*>& sinkBuddies = m_deviceAPI->getSinkBuddies();
    std::vector<DeviceAPI*>::const_iterator itSink = sinkBuddies.begin();

    qDebug("USRPOutput::suspendTxBuddies (%lu)", sinkBuddies.size());

    for (; itSink != sinkBuddies.end(); ++itSink)
    {
        DeviceUSRPShared *buddySharedPtr = (DeviceUSRPShared *) (*itSink)->getBuddySharedPtr();

        if (buddySharedPtr->m_thread && buddySharedPtr->m_thread->isRunning())
        {
            buddySharedPtr->m_thread->stopWork();
            buddySharedPtr->m_threadWasRunning = true;
        }
        else
        {
            buddySharedPtr->m_threadWasRunning = false;
        }
    }
}

void USRPOutput::resumeRxBuddies()
{
    const std::vector<DeviceAPI*>& sourceBuddies = m_deviceAPI->getSourceBuddies();
    std::vector<DeviceAPI*>::const_iterator itSource = sourceBuddies.begin();

    qDebug("USRPOutput::resumeRxBuddies (%lu)", sourceBuddies.size());

    for (; itSource != sourceBuddies.end(); ++itSource)
    {
        DeviceUSRPShared *buddySharedPtr = (DeviceUSRPShared *) (*itSource)->getBuddySharedPtr();

        if (buddySharedPtr->m_threadWasRunning) {
            buddySharedPtr->m_thread->startWork();
        }
    }
}

void USRPOutput::resumeTxBuddies()
{
    const std::vector<DeviceAPI*>& sinkBuddies = m_deviceAPI->getSinkBuddies();
    std::vector<DeviceAPI*>::const_iterator itSink = sinkBuddies.begin();

    qDebug("USRPOutput::resumeTxBuddies (%lu)", sinkBuddies.size());

    for (; itSink != sinkBuddies.end(); ++itSink)
    {
        DeviceUSRPShared *buddySharedPtr = (DeviceUSRPShared *) (*itSink)->getBuddySharedPtr();

        if (buddySharedPtr->m_threadWasRunning) {
            buddySharedPtr->m_thread->startWork();
        }
    }
}

void USRPOutput::closeDevice()
{
    if (m_deviceShared.m_deviceParams->getDevice() == nullptr) { // was never open
        return;
    }

    if (m_running) stop();

    // No buddies so effectively close the device

    if ((m_deviceAPI->getSourceBuddies().size() == 0) && (m_deviceAPI->getSinkBuddies().size() == 0))
    {
        m_deviceShared.m_deviceParams->close();
        delete m_deviceShared.m_deviceParams;
        m_deviceShared.m_deviceParams = 0;
    }

    m_deviceShared.m_channel = -1; // effectively release the channel for the possible buddies
}

bool USRPOutput::acquireChannel()
{
    suspendRxBuddies();
    suspendTxBuddies();

    try
    {
        // set up the stream
        std::string cpu_format("sc16");
        std::string wire_format("sc16");
        std::vector<size_t> channel_nums;
        channel_nums.push_back(m_deviceShared.m_channel);

        uhd::stream_args_t stream_args(cpu_format, wire_format);
        stream_args.channels = channel_nums;

        m_streamId = m_deviceShared.m_deviceParams->getDevice()->get_tx_stream(stream_args);
    }
    catch (std::exception& e)
    {
        qDebug() << "USRPOutput::acquireChannel: exception: " << e.what();
    }

    resumeTxBuddies();
    resumeRxBuddies();

    m_channelAcquired = true;

    return true;
}

void USRPOutput::releaseChannel()
{
    suspendRxBuddies();
    suspendTxBuddies();

    // destroy the stream - FIXME: Better way to do this?
    m_streamId = nullptr;

    resumeTxBuddies();
    resumeRxBuddies();

    m_channelAcquired = false;
}

void USRPOutput::init()
{
    applySettings(m_settings, true);
}

bool USRPOutput::start()
{
    if (!m_deviceShared.m_deviceParams->getDevice()) {
        return false;
    }

    if (m_running) { stop(); }

    if (!acquireChannel())
    {
        return false;
    }

    // start / stop streaming is done in the thread.

    m_usrpOutputThread = new USRPOutputThread(m_streamId, &m_sampleSourceFifo);
    qDebug("USRPOutput::start: thread created");

    applySettings(m_settings, true);

    m_usrpOutputThread->setLog2Interpolation(m_settings.m_log2SoftInterp);
    m_usrpOutputThread->startWork();

    m_deviceShared.m_thread = m_usrpOutputThread;
    m_running = true;

    return true;
}

void USRPOutput::stop()
{
    qDebug("USRPOutput::stop");

    if (m_usrpOutputThread)
    {
        m_usrpOutputThread->stopWork();
        delete m_usrpOutputThread;
        m_usrpOutputThread = nullptr;
    }

    m_deviceShared.m_thread = 0;
    m_running = false;

    releaseChannel();
}

QByteArray USRPOutput::serialize() const
{
    return m_settings.serialize();
}

bool USRPOutput::deserialize(const QByteArray& data)
{
    bool success = true;

    if (!m_settings.deserialize(data))
    {
        m_settings.resetToDefaults();
        success = false;
    }

    MsgConfigureUSRP* message = MsgConfigureUSRP::create(m_settings, true);
    m_inputMessageQueue.push(message);

    if (m_guiMessageQueue)
    {
        MsgConfigureUSRP* messageToGUI = MsgConfigureUSRP::create(m_settings, true);
        m_guiMessageQueue->push(messageToGUI);
    }

    return success;
}

const QString& USRPOutput::getDeviceDescription() const
{
    return m_deviceDescription;
}

int USRPOutput::getSampleRate() const
{
    int rate = m_settings.m_devSampleRate;
    return (rate / (1<<m_settings.m_log2SoftInterp));
}

quint64 USRPOutput::getCenterFrequency() const
{
    return m_settings.m_centerFrequency;
}

void USRPOutput::setCenterFrequency(qint64 centerFrequency)
{
    USRPOutputSettings settings = m_settings;
    settings.m_centerFrequency = centerFrequency;

    MsgConfigureUSRP* message = MsgConfigureUSRP::create(settings, false);
    m_inputMessageQueue.push(message);

    if (m_guiMessageQueue)
    {
        MsgConfigureUSRP* messageToGUI = MsgConfigureUSRP::create(settings, false);
        m_guiMessageQueue->push(messageToGUI);
    }
}

std::size_t USRPOutput::getChannelIndex()
{
    return m_deviceShared.m_channel;
}

void USRPOutput::getLORange(float& minF, float& maxF) const
{
    minF = m_deviceShared.m_deviceParams->m_loRangeTx.start();
    maxF = m_deviceShared.m_deviceParams->m_loRangeTx.stop();
}

void USRPOutput::getSRRange(float& minF, float& maxF) const
{
    minF = m_deviceShared.m_deviceParams->m_srRangeTx.start();
    maxF = m_deviceShared.m_deviceParams->m_srRangeTx.stop();
}

void USRPOutput::getLPRange(float& minF, float& maxF) const
{
    minF = m_deviceShared.m_deviceParams->m_lpfRangeTx.start();
    maxF = m_deviceShared.m_deviceParams->m_lpfRangeTx.stop();
}

void USRPOutput::getGainRange(float& minF, float& maxF) const
{
    minF = m_deviceShared.m_deviceParams->m_gainRangeTx.start();
    maxF = m_deviceShared.m_deviceParams->m_gainRangeTx.stop();
}

QStringList USRPOutput::getTxAntennas() const
{
    return m_deviceShared.m_deviceParams->m_txAntennas;
}

QStringList USRPOutput::getClockSources() const
{
    return m_deviceShared.m_deviceParams->m_clockSources;
}

bool USRPOutput::handleMessage(const Message& message)
{
    if (MsgConfigureUSRP::match(message))
    {
        MsgConfigureUSRP& conf = (MsgConfigureUSRP&) message;
        qDebug() << "USRPOutput::handleMessage: MsgConfigureUSRP";

        if (!applySettings(conf.getSettings(), conf.getForce()))
        {
            qDebug("USRPOutput::handleMessage config error");
        }

        return true;
    }
    else if (MsgStartStop::match(message))
    {
        MsgStartStop& cmd = (MsgStartStop&) message;
        qDebug() << "USRPOutput::handleMessage: MsgStartStop: " << (cmd.getStartStop() ? "start" : "stop");

        if (cmd.getStartStop())
        {
            if (m_deviceAPI->initDeviceEngine())
            {
                m_deviceAPI->startDeviceEngine();
            }
        }
        else
        {
            m_deviceAPI->stopDeviceEngine();
        }

        if (m_settings.m_useReverseAPI) {
            webapiReverseSendStartStop(cmd.getStartStop());
        }

        return true;
    }
    else if (DeviceUSRPShared::MsgReportBuddyChange::match(message))
    {
        DeviceUSRPShared::MsgReportBuddyChange& report = (DeviceUSRPShared::MsgReportBuddyChange&) message;

        if (report.getRxElseTx() && m_running)
        {
            double host_Hz;

            host_Hz = m_deviceShared.m_deviceParams->getDevice()->get_tx_rate(m_deviceShared.m_channel);
            m_settings.m_devSampleRate = roundf(host_Hz);

            qDebug() << "USRPOutput::handleMessage: MsgReportBuddyChange:"
                     << " m_devSampleRate: " << m_settings.m_devSampleRate;
        }
        else
        {
            m_settings.m_devSampleRate   = report.getDevSampleRate();
            m_settings.m_centerFrequency = report.getCenterFrequency();
        }

        DSPSignalNotification *notif = new DSPSignalNotification(
                m_settings.m_devSampleRate/(1<<m_settings.m_log2SoftInterp),
                m_settings.m_centerFrequency);
        m_deviceAPI->getDeviceEngineInputMessageQueue()->push(notif);

        DeviceUSRPShared::MsgReportBuddyChange *reportToGUI = DeviceUSRPShared::MsgReportBuddyChange::create(
                m_settings.m_devSampleRate, m_settings.m_centerFrequency, false);
        getMessageQueueToGUI()->push(reportToGUI);

        return true;
    }
    else if (DeviceUSRPShared::MsgReportClockSourceChange::match(message))
    {
        DeviceUSRPShared::MsgReportClockSourceChange& report = (DeviceUSRPShared::MsgReportClockSourceChange&) message;

        m_settings.m_clockSource = report.getClockSource();

        if (getMessageQueueToGUI())
        {
            DeviceUSRPShared::MsgReportClockSourceChange *reportToGUI = DeviceUSRPShared::MsgReportClockSourceChange::create(
                    m_settings.m_clockSource);
            getMessageQueueToGUI()->push(reportToGUI);
        }

        return true;
    }
    else if (MsgGetStreamInfo::match(message))
    {
        if (m_deviceAPI->getSamplingDeviceGUIMessageQueue())
        {
            if (m_streamId != nullptr)
            {
                bool active;
                quint32 underflows;
                quint32 droppedPackets;

                m_usrpOutputThread->getStreamStatus(active, underflows, droppedPackets);
                MsgReportStreamInfo *report = MsgReportStreamInfo::create(
                        true, // success
                        active,
                        underflows,
                        droppedPackets
                        );
                m_deviceAPI->getSamplingDeviceGUIMessageQueue()->push(report);
            }
            else
            {
                MsgReportStreamInfo *report = MsgReportStreamInfo::create(false, false, 0, 0);
                m_deviceAPI->getSamplingDeviceGUIMessageQueue()->push(report);
            }
        }

        return true;
    }
    else
    {
        return false;
    }
}

bool USRPOutput::applySettings(const USRPOutputSettings& settings, bool force)
{
    bool forwardChangeOwnDSP = false;
    bool forwardChangeTxDSP  = false;
    bool forwardChangeAllDSP = false;
    bool forwardClockSource  = false;
    bool ownThreadWasRunning = false;
    QList<QString> reverseAPIKeys;

    try
    {

        qint64 deviceCenterFrequency = settings.m_centerFrequency;
        deviceCenterFrequency -= settings.m_transverterMode ? settings.m_transverterDeltaFrequency : 0;
        deviceCenterFrequency = deviceCenterFrequency < 0 ? 0 : deviceCenterFrequency;

        // apply settings

        if ((m_settings.m_clockSource != settings.m_clockSource) || force)
        {
            reverseAPIKeys.append("clockSource");

            if (m_deviceShared.m_deviceParams->getDevice())
            {
                try
                {
                    m_deviceShared.m_deviceParams->getDevice()->set_clock_source(settings.m_clockSource.toStdString(), 0);
                    forwardClockSource = true;
                    qDebug() << "USRPOutput::applySettings: clock set to" << settings.m_clockSource;
                }
                catch (std::exception &e)
                {
                    // An exception will be thrown if the clock is not detected
                    // however, get_clock_source called below will still say the clock has is set
                    qCritical() << "USRPOutput::applySettings: could not set clock " << settings.m_clockSource;
                    // So, default back to internal
                    m_deviceShared.m_deviceParams->getDevice()->set_clock_source("internal", 0);
                    // notify GUI that source couldn't be set
                    forwardClockSource = true;
                }
            }
            else
            {
                qCritical() << "USRPOutput::applySettings: could not set clock to " << settings.m_clockSource;
            }
        }

        if ((m_settings.m_devSampleRate != settings.m_devSampleRate) || force)
        {
            reverseAPIKeys.append("devSampleRate");
            forwardChangeAllDSP = true;

            if (m_deviceShared.m_deviceParams->getDevice() && m_channelAcquired)
            {
                m_deviceShared.m_deviceParams->getDevice()->set_tx_rate(settings.m_devSampleRate, m_deviceShared.m_channel);
                double actualSampleRate = m_deviceShared.m_deviceParams->getDevice()->get_tx_rate(m_deviceShared.m_channel);
                qDebug("USRPOutput::applySettings: set sample rate set to %d - actual rate %f", settings.m_devSampleRate,
                        actualSampleRate);
                m_deviceShared.m_deviceParams->m_sampleRate = m_settings.m_devSampleRate;
            }
        }

        if ((m_settings.m_centerFrequency != settings.m_centerFrequency)
            || (m_settings.m_transverterMode != settings.m_transverterMode)
            || (m_settings.m_transverterDeltaFrequency != settings.m_transverterDeltaFrequency)
            || force)
        {
            reverseAPIKeys.append("centerFrequency");
            reverseAPIKeys.append("transverterMode");
            reverseAPIKeys.append("transverterDeltaFrequency");
            forwardChangeTxDSP = true;

            if (m_deviceShared.m_deviceParams->getDevice() && m_channelAcquired)
            {
                uhd::tune_request_t tune_request(deviceCenterFrequency);
                m_deviceShared.m_deviceParams->getDevice()->set_tx_freq(tune_request, m_deviceShared.m_channel);
                m_deviceShared.m_centerFrequency = deviceCenterFrequency; // for buddies
                qDebug("USRPOutput::applySettings: frequency set to %lld", deviceCenterFrequency);
            }
        }

        if ((m_settings.m_devSampleRate != settings.m_devSampleRate)
           || (m_settings.m_log2SoftInterp != settings.m_log2SoftInterp) || force)
        {
            reverseAPIKeys.append("devSampleRate");
            reverseAPIKeys.append("log2SoftInterp");

#if defined(_MSC_VER)
            unsigned int fifoRate = (unsigned int) settings.m_devSampleRate / (1<<settings.m_log2SoftInterp);
            fifoRate = fifoRate < 48000U ? 48000U : fifoRate;
#else
            unsigned int fifoRate = std::max(
                (unsigned int) settings.m_devSampleRate / (1<<settings.m_log2SoftInterp),
                DeviceUSRPShared::m_sampleFifoMinRate);
#endif
            m_sampleSourceFifo.resize(SampleSourceFifo::getSizePolicy(fifoRate));
            qDebug("USRPOutput::applySettings: resize FIFO: rate %u", fifoRate);
        }

        if ((m_settings.m_gain != settings.m_gain) || force)
        {
            reverseAPIKeys.append("gain");

            if (m_deviceShared.m_deviceParams->getDevice() && m_channelAcquired)
            {
                m_deviceShared.m_deviceParams->getDevice()->set_tx_gain(settings.m_gain, m_deviceShared.m_channel);
                qDebug() << "USRPOutput::applySettings: Gain set to " << settings.m_gain;
            }
        }

        if ((m_settings.m_lpfBW != settings.m_lpfBW) || force)
        {
            reverseAPIKeys.append("lpfBW");

            if (m_deviceShared.m_deviceParams->getDevice() && m_channelAcquired)
            {
                m_deviceShared.m_deviceParams->getDevice()->set_tx_bandwidth(settings.m_lpfBW, m_deviceShared.m_channel);
                qDebug("USRPOutput::applySettings: LPF BW: %f for channel %d", settings.m_lpfBW, m_deviceShared.m_channel);
            }
        }

        if ((m_settings.m_log2SoftInterp != settings.m_log2SoftInterp) || force)
        {
            reverseAPIKeys.append("log2SoftInterp");
            forwardChangeOwnDSP = true;
            m_deviceShared.m_log2Soft = settings.m_log2SoftInterp; // for buddies

            if (m_usrpOutputThread)
            {
                m_usrpOutputThread->setLog2Interpolation(settings.m_log2SoftInterp);
                qDebug() << "USRPOutput::applySettings: set soft interpolation to " << (1<<settings.m_log2SoftInterp);
            }
        }

        if ((m_settings.m_antennaPath != settings.m_antennaPath) || force)
        {
            reverseAPIKeys.append("antennaPath");

            if (m_deviceShared.m_deviceParams->getDevice() && m_channelAcquired)
            {
                m_deviceShared.m_deviceParams->getDevice()->set_tx_antenna(settings.m_antennaPath.toStdString(), m_deviceShared.m_channel);
                qDebug("USRPOutput::applySettings: set antenna path to %s on channel %d", qPrintable(settings.m_antennaPath), m_deviceShared.m_channel);
            }
        }

        if (settings.m_useReverseAPI)
        {
            bool fullUpdate = ((m_settings.m_useReverseAPI != settings.m_useReverseAPI) && settings.m_useReverseAPI) ||
                    (m_settings.m_reverseAPIAddress != settings.m_reverseAPIAddress) ||
                    (m_settings.m_reverseAPIPort != settings.m_reverseAPIPort) ||
                    (m_settings.m_reverseAPIDeviceIndex != settings.m_reverseAPIDeviceIndex);
            webapiReverseSendSettings(reverseAPIKeys, settings, fullUpdate || force);
        }

        m_settings = settings;

        // forward changes to buddies or oneself

        if (forwardChangeAllDSP)
        {
            qDebug("USRPOutput::applySettings: forward change to all buddies");

            // send to self first
            DSPSignalNotification *notif = new DSPSignalNotification(
                    m_settings.m_devSampleRate/(1<<m_settings.m_log2SoftInterp),
                    m_settings.m_centerFrequency);
            m_deviceAPI->getDeviceEngineInputMessageQueue()->push(notif);

            // send to sink buddies
            const std::vector<DeviceAPI*>& sinkBuddies = m_deviceAPI->getSinkBuddies();
            std::vector<DeviceAPI*>::const_iterator itSink = sinkBuddies.begin();

            for (; itSink != sinkBuddies.end(); ++itSink)
            {
                DeviceUSRPShared::MsgReportBuddyChange *report = DeviceUSRPShared::MsgReportBuddyChange::create(
                        m_settings.m_devSampleRate, m_settings.m_centerFrequency, false);
                (*itSink)->getSamplingDeviceInputMessageQueue()->push(report);
            }

            // send to source buddies
            const std::vector<DeviceAPI*>& sourceBuddies = m_deviceAPI->getSourceBuddies();
            std::vector<DeviceAPI*>::const_iterator itSource = sourceBuddies.begin();

            for (; itSource != sourceBuddies.end(); ++itSource)
            {
                DeviceUSRPShared::MsgReportBuddyChange *report = DeviceUSRPShared::MsgReportBuddyChange::create(
                        m_settings.m_devSampleRate, m_settings.m_centerFrequency, false);
                (*itSource)->getSamplingDeviceInputMessageQueue()->push(report);
            }
        }
        else if (forwardChangeTxDSP)
        {
            qDebug("USRPOutput::applySettings: forward change to Tx buddies");

            int sampleRate = m_settings.m_devSampleRate/(1<<m_settings.m_log2SoftInterp);

            // send to self first
            DSPSignalNotification *notif = new DSPSignalNotification(sampleRate, m_settings.m_centerFrequency);
            m_deviceAPI->getDeviceEngineInputMessageQueue()->push(notif);

            // send to sink buddies
            const std::vector<DeviceAPI*>& sinkBuddies = m_deviceAPI->getSinkBuddies();
            std::vector<DeviceAPI*>::const_iterator itSink = sinkBuddies.begin();

            for (; itSink != sinkBuddies.end(); ++itSink)
            {
                DeviceUSRPShared::MsgReportBuddyChange *report = DeviceUSRPShared::MsgReportBuddyChange::create(
                        m_settings.m_devSampleRate, m_settings.m_centerFrequency, false);
                (*itSink)->getSamplingDeviceInputMessageQueue()->push(report);
            }
        }
        else if (forwardChangeOwnDSP)
        {
            qDebug("USRPOutput::applySettings: forward change to self only");

            int sampleRate = m_settings.m_devSampleRate/(1<<m_settings.m_log2SoftInterp);
            DSPSignalNotification *notif = new DSPSignalNotification(sampleRate, m_settings.m_centerFrequency);
            m_deviceAPI->getDeviceEngineInputMessageQueue()->push(notif);
        }

        if (forwardClockSource)
        {
            // get what clock is actually set, in case requested clock couldn't be set
            if (m_deviceShared.m_deviceParams->getDevice())
            {
                try
                {
                    m_settings.m_clockSource = QString::fromStdString(m_deviceShared.m_deviceParams->getDevice()->get_clock_source(0));
                    qDebug() << "USRPOutput::applySettings: clock source is " << m_settings.m_clockSource;
                }
                catch (std::exception &e)
                {
                    qDebug() << "USRPOutput::applySettings: could not get clock source";
                }
            }

            // send to source buddies
            const std::vector<DeviceAPI*>& sourceBuddies = m_deviceAPI->getSourceBuddies();
            std::vector<DeviceAPI*>::const_iterator itSource = sourceBuddies.begin();

            for (; itSource != sourceBuddies.end(); ++itSource)
            {
                DeviceUSRPShared::MsgReportClockSourceChange *report = DeviceUSRPShared::MsgReportClockSourceChange::create(
                        m_settings.m_clockSource);
                (*itSource)->getSamplingDeviceInputMessageQueue()->push(report);
            }

            // send to sink buddies
            const std::vector<DeviceAPI*>& sinkBuddies = m_deviceAPI->getSinkBuddies();
            std::vector<DeviceAPI*>::const_iterator itSink = sinkBuddies.begin();

            for (; itSink != sinkBuddies.end(); ++itSink)
            {
                DeviceUSRPShared::MsgReportClockSourceChange *report = DeviceUSRPShared::MsgReportClockSourceChange::create(
                        m_settings.m_clockSource);
                (*itSink)->getSamplingDeviceInputMessageQueue()->push(report);
            }
        }

        QLocale loc;

        qDebug().noquote() << "USRPOutput::applySettings: center freq: " << m_settings.m_centerFrequency << " Hz"
                << " m_transverterMode: " << m_settings.m_transverterMode
                << " m_transverterDeltaFrequency: " << m_settings.m_transverterDeltaFrequency
                << " deviceCenterFrequency: " << deviceCenterFrequency
                << " device stream sample rate: " << loc.toString(m_settings.m_devSampleRate) << "S/s"
                << " sample rate with soft interpolation: " << loc.toString( m_settings.m_devSampleRate/(1<<m_settings.m_log2SoftInterp)) << "S/s"
                << " m_log2SoftInterp: " << m_settings.m_log2SoftInterp
                << " m_gain: " << m_settings.m_gain
                << " m_lpfBW: " << loc.toString(static_cast<int>(m_settings.m_lpfBW))
                << " m_antennaPath: " << m_settings.m_antennaPath
                << " m_clockSource: " << m_settings.m_clockSource
                << " force: " << force;

        return true;
    }
    catch (std::exception &e)
    {
        qDebug() << "USRPOutput::applySettings: exception: " << e.what();
        return false;
    }
}

int USRPOutput::webapiSettingsGet(
                SWGSDRangel::SWGDeviceSettings& response,
                QString& errorMessage)
{
    (void) errorMessage;
    response.setUsrpOutputSettings(new SWGSDRangel::SWGUSRPOutputSettings());
    response.getUsrpOutputSettings()->init();
    webapiFormatDeviceSettings(response, m_settings);
    return 200;
}

int USRPOutput::webapiSettingsPutPatch(
                bool force,
                const QStringList& deviceSettingsKeys,
                SWGSDRangel::SWGDeviceSettings& response, // query + response
                QString& errorMessage)
{
    (void) errorMessage;
    USRPOutputSettings settings = m_settings;
    webapiUpdateDeviceSettings(settings, deviceSettingsKeys, response);

    MsgConfigureUSRP *msg = MsgConfigureUSRP::create(settings, force);
    m_inputMessageQueue.push(msg);

    if (m_guiMessageQueue) // forward to GUI if any
    {
        MsgConfigureUSRP *msgToGUI = MsgConfigureUSRP::create(settings, force);
        m_guiMessageQueue->push(msgToGUI);
    }

    webapiFormatDeviceSettings(response, settings);
    return 200;
}

void USRPOutput::webapiUpdateDeviceSettings(
        USRPOutputSettings& settings,
        const QStringList& deviceSettingsKeys,
        SWGSDRangel::SWGDeviceSettings& response)
{
    if (deviceSettingsKeys.contains("antennaPath")) {
        settings.m_antennaPath = *response.getUsrpOutputSettings()->getAntennaPath();
    }
    if (deviceSettingsKeys.contains("centerFrequency")) {
        settings.m_centerFrequency = response.getUsrpOutputSettings()->getCenterFrequency();
    }
    if (deviceSettingsKeys.contains("devSampleRate")) {
        settings.m_devSampleRate = response.getUsrpOutputSettings()->getDevSampleRate();
    }
    if (deviceSettingsKeys.contains("clockSource")) {
        settings.m_clockSource = *response.getUsrpOutputSettings()->getClockSource();
    }
    if (deviceSettingsKeys.contains("gain")) {
        settings.m_gain = response.getUsrpOutputSettings()->getGain();
    }
    if (deviceSettingsKeys.contains("log2SoftInterp")) {
        settings.m_log2SoftInterp = response.getUsrpOutputSettings()->getLog2SoftInterp();
    }
    if (deviceSettingsKeys.contains("lpfBW")) {
        settings.m_lpfBW = response.getUsrpOutputSettings()->getLpfBw();
    }
    if (deviceSettingsKeys.contains("transverterDeltaFrequency")) {
        settings.m_transverterDeltaFrequency = response.getUsrpOutputSettings()->getTransverterDeltaFrequency();
    }
    if (deviceSettingsKeys.contains("transverterMode")) {
        settings.m_transverterMode = response.getUsrpOutputSettings()->getTransverterMode() != 0;
    }
    if (deviceSettingsKeys.contains("useReverseAPI")) {
        settings.m_useReverseAPI = response.getUsrpOutputSettings()->getUseReverseApi() != 0;
    }
    if (deviceSettingsKeys.contains("reverseAPIAddress")) {
        settings.m_reverseAPIAddress = *response.getUsrpOutputSettings()->getReverseApiAddress();
    }
    if (deviceSettingsKeys.contains("reverseAPIPort")) {
        settings.m_reverseAPIPort = response.getUsrpOutputSettings()->getReverseApiPort();
    }
    if (deviceSettingsKeys.contains("reverseAPIDeviceIndex")) {
        settings.m_reverseAPIDeviceIndex = response.getUsrpOutputSettings()->getReverseApiDeviceIndex();
    }
}

int USRPOutput::webapiReportGet(
        SWGSDRangel::SWGDeviceReport& response,
        QString& errorMessage)
{
    (void) errorMessage;
    response.setUsrpOutputReport(new SWGSDRangel::SWGUSRPOutputReport());
    response.getUsrpOutputReport()->init();
    webapiFormatDeviceReport(response);
    return 200;
}
void USRPOutput::webapiFormatDeviceSettings(SWGSDRangel::SWGDeviceSettings& response, const USRPOutputSettings& settings)
{
    response.getUsrpOutputSettings()->setAntennaPath(new QString(settings.m_antennaPath));
    response.getUsrpOutputSettings()->setCenterFrequency(settings.m_centerFrequency);
    response.getUsrpOutputSettings()->setDevSampleRate(settings.m_devSampleRate);
    response.getUsrpOutputSettings()->setClockSource(new QString(settings.m_clockSource));
    response.getUsrpOutputSettings()->setGain(settings.m_gain);
    response.getUsrpOutputSettings()->setLog2SoftInterp(settings.m_log2SoftInterp);
    response.getUsrpOutputSettings()->setLpfBw(settings.m_lpfBW);
    response.getUsrpOutputSettings()->setTransverterDeltaFrequency(settings.m_transverterDeltaFrequency);
    response.getUsrpOutputSettings()->setTransverterMode(settings.m_transverterMode ? 1 : 0);
    response.getUsrpOutputSettings()->setUseReverseApi(settings.m_useReverseAPI ? 1 : 0);

    if (response.getUsrpOutputSettings()->getReverseApiAddress()) {
        *response.getUsrpOutputSettings()->getReverseApiAddress() = settings.m_reverseAPIAddress;
    } else {
        response.getUsrpOutputSettings()->setReverseApiAddress(new QString(settings.m_reverseAPIAddress));
    }

    response.getUsrpOutputSettings()->setReverseApiPort(settings.m_reverseAPIPort);
    response.getUsrpOutputSettings()->setReverseApiDeviceIndex(settings.m_reverseAPIDeviceIndex);
}

int USRPOutput::webapiRunGet(
        SWGSDRangel::SWGDeviceState& response,
        QString& errorMessage)
{
    (void) errorMessage;
    m_deviceAPI->getDeviceEngineStateStr(*response.getState());
    return 200;
}

int USRPOutput::webapiRun(
        bool run,
        SWGSDRangel::SWGDeviceState& response,
        QString& errorMessage)
{
    (void) errorMessage;
    m_deviceAPI->getDeviceEngineStateStr(*response.getState());
    MsgStartStop *message = MsgStartStop::create(run);
    m_inputMessageQueue.push(message);

    if (m_guiMessageQueue)
    {
        MsgStartStop *messagetoGui = MsgStartStop::create(run);
        m_guiMessageQueue->push(messagetoGui);
    }

    return 200;
}

void USRPOutput::webapiFormatDeviceReport(SWGSDRangel::SWGDeviceReport& response)
{
    bool success = false;
    bool active = false;
    quint32 underflows = 0;
    quint32 droppedPackets = 0;

    if (m_streamId != nullptr)
    {
        m_usrpOutputThread->getStreamStatus(active, underflows, droppedPackets);
        success = true;
    }

    response.getUsrpOutputReport()->setSuccess(success ? 1 : 0);
    response.getUsrpOutputReport()->setStreamActive(active ? 1 : 0);
    response.getUsrpOutputReport()->setUnderrunCount(underflows);
    response.getUsrpOutputReport()->setDroppedPacketsCount(droppedPackets);
}

void USRPOutput::webapiReverseSendSettings(QList<QString>& deviceSettingsKeys, const USRPOutputSettings& settings, bool force)
{
    SWGSDRangel::SWGDeviceSettings *swgDeviceSettings = new SWGSDRangel::SWGDeviceSettings();
    swgDeviceSettings->setDirection(1); // single Tx
    swgDeviceSettings->setOriginatorIndex(m_deviceAPI->getDeviceSetIndex());
    swgDeviceSettings->setDeviceHwType(new QString("USRP"));
    swgDeviceSettings->setUsrpOutputSettings(new SWGSDRangel::SWGUSRPOutputSettings());
    SWGSDRangel::SWGUSRPOutputSettings *swgUsrpOutputSettings = swgDeviceSettings->getUsrpOutputSettings();

    // transfer data that has been modified. When force is on transfer all data except reverse API data

    if (deviceSettingsKeys.contains("antennaPath") || force) {
        swgUsrpOutputSettings->setAntennaPath(new QString(settings.m_antennaPath));
    }
    if (deviceSettingsKeys.contains("centerFrequency") || force) {
        swgUsrpOutputSettings->setCenterFrequency(settings.m_centerFrequency);
    }
    if (deviceSettingsKeys.contains("devSampleRate") || force) {
        swgUsrpOutputSettings->setDevSampleRate(settings.m_devSampleRate);
    }
    if (deviceSettingsKeys.contains("clockSource") || force) {
        swgUsrpOutputSettings->setClockSource(new QString(settings.m_clockSource));
    }
    if (deviceSettingsKeys.contains("gain") || force) {
        swgUsrpOutputSettings->setGain(settings.m_gain);
    }
    if (deviceSettingsKeys.contains("log2SoftInterp") || force) {
        swgUsrpOutputSettings->setLog2SoftInterp(settings.m_log2SoftInterp);
    }
    if (deviceSettingsKeys.contains("lpfBW") || force) {
        swgUsrpOutputSettings->setLpfBw(settings.m_lpfBW);
    }
    if (deviceSettingsKeys.contains("transverterDeltaFrequency") || force) {
        swgUsrpOutputSettings->setTransverterDeltaFrequency(settings.m_transverterDeltaFrequency);
    }
    if (deviceSettingsKeys.contains("transverterMode") || force) {
        swgUsrpOutputSettings->setTransverterMode(settings.m_transverterMode ? 1 : 0);
    }

    QString deviceSettingsURL = QString("http://%1:%2/sdrangel/deviceset/%3/device/settings")
            .arg(settings.m_reverseAPIAddress)
            .arg(settings.m_reverseAPIPort)
            .arg(settings.m_reverseAPIDeviceIndex);
    m_networkRequest.setUrl(QUrl(deviceSettingsURL));
    m_networkRequest.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");

    QBuffer *buffer = new QBuffer();
    buffer->open((QBuffer::ReadWrite));
    buffer->write(swgDeviceSettings->asJson().toUtf8());
    buffer->seek(0);

    // Always use PATCH to avoid passing reverse API settings
    QNetworkReply *reply = m_networkManager->sendCustomRequest(m_networkRequest, "PATCH", buffer);
    buffer->setParent(reply);

    delete swgDeviceSettings;
}

void USRPOutput::webapiReverseSendStartStop(bool start)
{
    SWGSDRangel::SWGDeviceSettings *swgDeviceSettings = new SWGSDRangel::SWGDeviceSettings();
    swgDeviceSettings->setDirection(1); // single Tx
    swgDeviceSettings->setOriginatorIndex(m_deviceAPI->getDeviceSetIndex());
    swgDeviceSettings->setDeviceHwType(new QString("USRP"));

    QString deviceSettingsURL = QString("http://%1:%2/sdrangel/deviceset/%3/device/run")
            .arg(m_settings.m_reverseAPIAddress)
            .arg(m_settings.m_reverseAPIPort)
            .arg(m_settings.m_reverseAPIDeviceIndex);
    m_networkRequest.setUrl(QUrl(deviceSettingsURL));
    m_networkRequest.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");

    QBuffer *buffer = new QBuffer();
    buffer->open((QBuffer::ReadWrite));
    buffer->write(swgDeviceSettings->asJson().toUtf8());
    buffer->seek(0);
    QNetworkReply *reply;

    if (start) {
        reply = m_networkManager->sendCustomRequest(m_networkRequest, "POST", buffer);
    } else {
        reply = m_networkManager->sendCustomRequest(m_networkRequest, "DELETE", buffer);
    }

    buffer->setParent(reply);
    delete swgDeviceSettings;
}

void USRPOutput::networkManagerFinished(QNetworkReply *reply)
{
    QNetworkReply::NetworkError replyError = reply->error();

    if (replyError)
    {
        qWarning() << "USRPOutput::networkManagerFinished:"
                << " error(" << (int) replyError
                << "): " << replyError
                << ": " << reply->errorString();
    }
    else
    {
        QString answer = reply->readAll();
        answer.chop(1); // remove last \n
        qDebug("USRPOutput::networkManagerFinished: reply:\n%s", answer.toStdString().c_str());
    }

    reply->deleteLater();
}