/*
 * Copyright (C) 2021 Huawei Device Co., Ltd.
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include "usb.h"

namespace Hdc {
HdcUSBBase::HdcUSBBase(const bool serverOrDaemonIn, void *ptrMainBase)
{
    serverOrDaemon = serverOrDaemonIn;
    clsMainBase = ptrMainBase;
    modRunning = true;
}

HdcUSBBase::~HdcUSBBase()
{
}

void HdcUSBBase::ReadUSB(uv_stream_t *stream, ssize_t nread, const uv_buf_t *buf)
{
    HSession hSession = (HSession)stream->data;
    HdcSessionBase *hSessionBase = (HdcSessionBase *)hSession->classInstance;
    if (hSessionBase->FetchIOBuf(hSession, hSession->ioBuf, nread) < 0) {
        hSessionBase->FreeSession(hSession->sessionId);
    }
}

bool HdcUSBBase::ReadyForWorkThread(HSession hSession)
{
    // Server-end USB IO is handed over to each sub-thread, only the daemon is still read by the main IO to distribute
    // to each sub-thread by DataPipe.
    if (uv_tcp_init(&hSession->childLoop, &hSession->dataPipe[STREAM_WORK])
        || uv_tcp_open(&hSession->dataPipe[STREAM_WORK], hSession->dataFd[STREAM_WORK])) {
        WRITE_LOG(LOG_FATAL, "USBBase ReadyForWorkThread init child TCP failed");
        return false;
    }
    hSession->dataPipe[STREAM_WORK].data = hSession;
    HdcSessionBase *pSession = (HdcSessionBase *)hSession->classInstance;
    Base::SetTcpOptions(&hSession->dataPipe[STREAM_WORK]);
    if (uv_read_start((uv_stream_t *)&hSession->dataPipe[STREAM_WORK], pSession->AllocCallback, ReadUSB)) {
        WRITE_LOG(LOG_FATAL, "USBBase ReadyForWorkThread child TCP read failed");
        return false;
    }
    WRITE_LOG(LOG_DEBUG, "USBBase ReadyForWorkThread finish");
    return true;
};

// USB big data stream, block transmission, mainly to prevent accidental data packets from writing through EP port,
// inserting the send queue causes the program to crash
int HdcUSBBase::SendUSBBlock(HSession hSession, uint8_t *data, const int length)
{
    //  Format:USBPacket1 payload1...USBPacketn payloadn；
    //  [USBHead1(PayloadHead1+Payload1)]+[USBHead2(Payload2)]+...+[USBHeadN(PayloadN)]
    //
    int ioStaticValue = std::min(Base::GetMaxBufSize(), Base::GetUsbffsMaxBulkSize());
    // Get integer division maximum and the data size is aligned
    int maxIOSize = ioStaticValue - (ioStaticValue % hSession->hUSB->wMaxPacketSizeSend);
    int sizeUSBPacketHead = sizeof(USBHead);
    int singleSize = maxIOSize - sizeUSBPacketHead;
    int iMod = length % singleSize;
    int iCount = (length - iMod) / singleSize + 1;
    int offset = 0;
    int dataSize = 0;
    int childRet = 0;
    int i = 0;  // It doesn't matter of 0 or 1, start from 1 to send it according to the serial number.

    uint8_t *ioBuf = new uint8_t[maxIOSize]();
    if (!ioBuf) {
        return ERR_BUF_ALLOC;
    }
    for (i = 0; i < iCount; ++i) {
        USBHead *pUSBHead = (USBHead *)ioBuf;
        int errCode = memcpy_s(pUSBHead->flag, sizeof(pUSBHead->flag), PACKET_FLAG.c_str(), PACKET_FLAG.size());
        if (errCode != EOK) {
            offset = ERR_BUF_COPY;
            break;
        }
        pUSBHead->sessionId = htonl(hSession->sessionId);
        if (i != iCount - 1) {
            dataSize = singleSize;
        } else {
            dataSize = iMod;
            pUSBHead->option = pUSBHead->option | USB_OPTION_TAIL;
        }
        pUSBHead->dataSize = htons(static_cast<uint16_t>(dataSize));
        uint8_t *payload = ioBuf + sizeUSBPacketHead;
        if (EOK != memcpy_s(payload, maxIOSize - sizeUSBPacketHead, (uint8_t *)data + offset, dataSize)) {
            offset = ERR_BUF_COPY;
            break;
        }
        offset += dataSize;
        ++hSession->ref;
        if ((childRet = SendUSBRaw(hSession, ioBuf, sizeUSBPacketHead + dataSize)) <= 0) {
            offset = ERR_IO_FAIL;
            break;
        }
        if (!hSession->serverOrDaemon && (childRet % hSession->hUSB->wMaxPacketSizeSend == 0)) {
            // Just daemon enable zero length packet.
            // win32 send ZLP will block winusb driver and LIBUSB_TRANSFER_ADD_ZERO_PACKET not effect
            uint8_t dummy = 0;
            SendUSBRaw(hSession, &dummy, 0);
        }
    }
    delete[] ioBuf;
    return offset;
}

// return value: <0 error; = 0 all finish; >0 need size
int HdcUSBBase::SendToHdcStream(HSession hSession, uv_stream_t *stream, uint8_t *appendData, int dataSize)
{
    HUSB hUSB = hSession->hUSB;
    vector<uint8_t> &bufRecv = hUSB->bufRecv;
    bufRecv.insert(bufRecv.end(), appendData, appendData + dataSize);
    int ret = RET_SUCCESS;
    while (bufRecv.size() >= sizeof(USBHead)) {
        USBHead *usbHeader = (USBHead *)bufRecv.data();
        if (memcmp(usbHeader->flag, PACKET_FLAG.c_str(), PACKET_FLAG.size())) {
            WRITE_LOG(LOG_FATAL, "Error usb packet");
            ret = ERR_BUF_CHECK;
            break;
        }
        usbHeader->sessionId = ntohl(usbHeader->sessionId);
        usbHeader->dataSize = ntohs(usbHeader->dataSize);
        if (usbHeader->dataSize > USBFFS_BULKSIZE_MAX) {
            ret = ERR_BUF_SIZE;
            break;
        }
        uint32_t fullPacketSize = sizeof(USBHead) + usbHeader->dataSize;
        if (bufRecv.size() < fullPacketSize) {
            WRITE_LOG(LOG_DEBUG, "SendToHdcStream not enough dataSize:%d bufRecvSize:%d", usbHeader->dataSize,
                      bufRecv.size());
            ret = fullPacketSize - bufRecv.size();
            break;  // successful , but not enough
        }
        if (usbHeader->sessionId != hSession->sessionId) {
            // Only server do it here, daemon 'SendUsbSoftReset' no use
            // hilog + ctrl^C to reproduction scene
            //
            // Because the USB-reset API does not work on all platforms, the last session IO data may be
            // recveived, we need to ignore it.
            if (hSession->serverOrDaemon && !hUSB->resetIO) {
                WRITE_LOG(LOG_WARN, "SendToHdcStream sessionId not matched");
                SendUsbSoftReset(hUSB, usbHeader->sessionId);
                hUSB->resetIO = true;
            }
        } else {
            // usb data to logic
            if (Base::SendToStream(stream, bufRecv.data() + sizeof(USBHead), usbHeader->dataSize) < 0) {
                ret = ERR_IO_FAIL;
                WRITE_LOG(LOG_FATAL, "Error usb send to stream dataSize:%d bufRecvSize:%d", usbHeader->dataSize,
                          bufRecv.size());
                break;
            }
        }
        bufRecv.erase(bufRecv.begin(), bufRecv.begin() + fullPacketSize);
    }
    return ret;
}
}