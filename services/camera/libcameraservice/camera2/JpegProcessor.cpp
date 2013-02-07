/*
 * Copyright (C) 2012 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define LOG_TAG "Camera2-JpegProcessor"
#define ATRACE_TAG ATRACE_TAG_CAMERA
//#define LOG_NDEBUG 0

#include <netinet/in.h>

#include <binder/MemoryBase.h>
#include <binder/MemoryHeapBase.h>
#include <utils/Log.h>
#include <utils/Trace.h>

#include "JpegProcessor.h"
#include <gui/Surface.h>
#include "../CameraDeviceBase.h"
#include "../Camera2Client.h"


namespace android {
namespace camera2 {

JpegProcessor::JpegProcessor(
    wp<Camera2Client> client,
    wp<CaptureSequencer> sequencer):
        Thread(false),
        mClient(client),
        mSequencer(sequencer),
        mCaptureAvailable(false),
        mCaptureStreamId(NO_STREAM) {
}

JpegProcessor::~JpegProcessor() {
    ALOGV("%s: Exit", __FUNCTION__);
    deleteStream();
}

void JpegProcessor::onFrameAvailable() {
    Mutex::Autolock l(mInputMutex);
    if (!mCaptureAvailable) {
        mCaptureAvailable = true;
        mCaptureAvailableSignal.signal();
    }
}

status_t JpegProcessor::updateStream(const Parameters &params) {
    ATRACE_CALL();
    ALOGV("%s", __FUNCTION__);
    status_t res;

    Mutex::Autolock l(mInputMutex);

    sp<Camera2Client> client = mClient.promote();
    if (client == 0) return OK;
    sp<CameraDeviceBase> device = client->getCameraDevice();

    // Find out buffer size for JPEG
    camera_metadata_ro_entry_t maxJpegSize =
            params.staticInfo(ANDROID_JPEG_MAX_SIZE);
    if (maxJpegSize.count == 0) {
        ALOGE("%s: Camera %d: Can't find ANDROID_JPEG_MAX_SIZE!",
                __FUNCTION__, client->getCameraId());
        return INVALID_OPERATION;
    }

    if (mCaptureConsumer == 0) {
        // Create CPU buffer queue endpoint
        mCaptureConsumer = new CpuConsumer(1);
        mCaptureConsumer->setFrameAvailableListener(this);
        mCaptureConsumer->setName(String8("Camera2Client::CaptureConsumer"));
        mCaptureWindow = new Surface(
            mCaptureConsumer->getProducerInterface());
        // Create memory for API consumption
        mCaptureHeap = new MemoryHeapBase(maxJpegSize.data.i32[0], 0,
                                       "Camera2Client::CaptureHeap");
        if (mCaptureHeap->getSize() == 0) {
            ALOGE("%s: Camera %d: Unable to allocate memory for capture",
                    __FUNCTION__, client->getCameraId());
            return NO_MEMORY;
        }
    }

    if (mCaptureStreamId != NO_STREAM) {
        // Check if stream parameters have to change
        uint32_t currentWidth, currentHeight;
        res = device->getStreamInfo(mCaptureStreamId,
                &currentWidth, &currentHeight, 0);
        if (res != OK) {
            ALOGE("%s: Camera %d: Error querying capture output stream info: "
                    "%s (%d)", __FUNCTION__,
                    client->getCameraId(), strerror(-res), res);
            return res;
        }
        if (currentWidth != (uint32_t)params.pictureWidth ||
                currentHeight != (uint32_t)params.pictureHeight) {
            ALOGV("%s: Camera %d: Deleting stream %d since the buffer dimensions changed",
                __FUNCTION__, client->getCameraId(), mCaptureStreamId);
            res = device->deleteStream(mCaptureStreamId);
            if (res != OK) {
                ALOGE("%s: Camera %d: Unable to delete old output stream "
                        "for capture: %s (%d)", __FUNCTION__,
                        client->getCameraId(), strerror(-res), res);
                return res;
            }
            mCaptureStreamId = NO_STREAM;
        }
    }

    if (mCaptureStreamId == NO_STREAM) {
        // Create stream for HAL production
        res = device->createStream(mCaptureWindow,
                params.pictureWidth, params.pictureHeight,
                HAL_PIXEL_FORMAT_BLOB, maxJpegSize.data.i32[0],
                &mCaptureStreamId);
        if (res != OK) {
            ALOGE("%s: Camera %d: Can't create output stream for capture: "
                    "%s (%d)", __FUNCTION__, client->getCameraId(),
                    strerror(-res), res);
            return res;
        }

    }
    return OK;
}

status_t JpegProcessor::deleteStream() {
    ATRACE_CALL();

    Mutex::Autolock l(mInputMutex);

    if (mCaptureStreamId != NO_STREAM) {
        sp<Camera2Client> client = mClient.promote();
        if (client == 0) return OK;
        sp<CameraDeviceBase> device = client->getCameraDevice();

        device->deleteStream(mCaptureStreamId);

        mCaptureHeap.clear();
        mCaptureWindow.clear();
        mCaptureConsumer.clear();

        mCaptureStreamId = NO_STREAM;
    }
    return OK;
}

int JpegProcessor::getStreamId() const {
    Mutex::Autolock l(mInputMutex);
    return mCaptureStreamId;
}

void JpegProcessor::dump(int /*fd*/, const Vector<String16>& /*args*/) const {
}

bool JpegProcessor::threadLoop() {
    status_t res;

    {
        Mutex::Autolock l(mInputMutex);
        while (!mCaptureAvailable) {
            res = mCaptureAvailableSignal.waitRelative(mInputMutex,
                    kWaitDuration);
            if (res == TIMED_OUT) return true;
        }
        mCaptureAvailable = false;
    }

    do {
        sp<Camera2Client> client = mClient.promote();
        if (client == 0) return false;
        res = processNewCapture(client);
    } while (res == OK);

    return true;
}

status_t JpegProcessor::processNewCapture(sp<Camera2Client> &client) {
    ATRACE_CALL();
    status_t res;
    sp<Camera2Heap> captureHeap;

    CpuConsumer::LockedBuffer imgBuffer;

    res = mCaptureConsumer->lockNextBuffer(&imgBuffer);
    if (res != OK) {
        if (res != BAD_VALUE) {
            ALOGE("%s: Camera %d: Error receiving still image buffer: "
                    "%s (%d)", __FUNCTION__,
                    client->getCameraId(), strerror(-res), res);
        }
        return res;
    }

    ALOGV("%s: Camera %d: Still capture available", __FUNCTION__,
            client->getCameraId());

    if (imgBuffer.format != HAL_PIXEL_FORMAT_BLOB) {
        ALOGE("%s: Camera %d: Unexpected format for still image: "
                "%x, expected %x", __FUNCTION__, client->getCameraId(),
                imgBuffer.format,
                HAL_PIXEL_FORMAT_BLOB);
        mCaptureConsumer->unlockBuffer(imgBuffer);
        return OK;
    }

    // Find size of JPEG image
    size_t jpegSize = findJpegSize(imgBuffer.data, imgBuffer.width);
    if (jpegSize == 0) { // failed to find size, default to whole buffer
        jpegSize = imgBuffer.width;
    }
    size_t heapSize = mCaptureHeap->getSize();
    if (jpegSize > heapSize) {
        ALOGW("%s: JPEG image is larger than expected, truncating "
                "(got %d, expected at most %d bytes)",
                __FUNCTION__, jpegSize, heapSize);
        jpegSize = heapSize;
    }

    // TODO: Optimize this to avoid memcopy
    sp<MemoryBase> captureBuffer = new MemoryBase(mCaptureHeap, 0, jpegSize);
    void* captureMemory = mCaptureHeap->getBase();
    memcpy(captureMemory, imgBuffer.data, jpegSize);

    mCaptureConsumer->unlockBuffer(imgBuffer);

    sp<CaptureSequencer> sequencer = mSequencer.promote();
    if (sequencer != 0) {
        sequencer->onCaptureAvailable(imgBuffer.timestamp, captureBuffer);
    }

    return OK;
}

/*
 * JPEG FILE FORMAT OVERVIEW.
 * http://www.jpeg.org/public/jfif.pdf
 * (JPEG is the image compression algorithm, actual file format is called JFIF)
 *
 * "Markers" are 2-byte patterns used to distinguish parts of JFIF files.  The
 * first byte is always 0xFF, and the second byte is between 0x01 and 0xFE
 * (inclusive).  Because every marker begins with the same byte, they are
 * referred to by the second byte's value.
 *
 * JFIF files all begin with the Start of Image (SOI) marker, which is 0xD8.
 * Following it, "segment" sections begin with other markers, followed by a
 * 2-byte length (in network byte order), then the segment data.
 *
 * For our purposes we will ignore the data, and just use the length to skip to
 * the next segment.  This is necessary because the data inside segments are
 * allowed to contain the End of Image marker (0xFF 0xD9), preventing us from
 * naievely scanning until the end.
 *
 * After all the segments are processed, the jpeg compressed image stream begins.
 * This can be considered an opaque format with one requirement: all 0xFF bytes
 * in this stream must be followed with a 0x00 byte.  This prevents any of the
 * image data to be interpreted as a segment.  The only exception to this is at
 * the end of the image stream there is an End of Image (EOI) marker, which is
 * 0xFF followed by a non-zero (0xD9) byte.
 */

const uint8_t MARK = 0xFF; // First byte of marker
const uint8_t SOI = 0xD8; // Start of Image
const uint8_t EOI = 0xD9; // End of Image
const size_t MARKER_LENGTH = 2; // length of a marker

#pragma pack(push)
#pragma pack(1)
typedef struct segment {
    uint8_t marker[MARKER_LENGTH];
    uint16_t length;
} segment_t;
#pragma pack(pop)

/* HELPER FUNCTIONS */

// check for Start of Image marker
bool checkJpegStart(uint8_t* buf) {
    return buf[0] == MARK && buf[1] == SOI;
}
// check for End of Image marker
bool checkJpegEnd(uint8_t *buf) {
    return buf[0] == MARK && buf[1] == EOI;
}
// check for arbitrary marker, returns marker type (second byte)
// returns 0 if no marker found. Note: 0x00 is not a valid marker type
uint8_t checkJpegMarker(uint8_t *buf) {
    if (buf[0] == MARK && buf[1] > 0 && buf[1] < 0xFF) {
        return buf[1];
    }
    return 0;
}

// Return the size of the JPEG, 0 indicates failure
size_t JpegProcessor::findJpegSize(uint8_t* jpegBuffer, size_t maxSize) {
    size_t size;

    // First check for JPEG transport header at the end of the buffer
    uint8_t *header = jpegBuffer + (maxSize - sizeof(struct camera2_jpeg_blob));
    struct camera2_jpeg_blob *blob = (struct camera2_jpeg_blob*)(header);
    if (blob->jpeg_blob_id == CAMERA2_JPEG_BLOB_ID) {
        size = blob->jpeg_size;
        if (size > 0 && size <= maxSize - sizeof(struct camera2_jpeg_blob)) {
            // Verify SOI and EOI markers
            size_t offset = size - MARKER_LENGTH;
            uint8_t *end = jpegBuffer + offset;
            if (checkJpegStart(jpegBuffer) && checkJpegEnd(end)) {
                ALOGV("Found JPEG transport header, img size %d", size);
                return size;
            } else {
                ALOGW("Found JPEG transport header with bad Image Start/End");
            }
        } else {
            ALOGW("Found JPEG transport header with bad size %d", size);
        }
    }

    // Check Start of Image
    if ( !checkJpegStart(jpegBuffer) ) {
        ALOGE("Could not find start of JPEG marker");
        return 0;
    }

    // Read JFIF segment markers, skip over segment data
    size = 0;
    while (size <= maxSize - MARKER_LENGTH) {
        segment_t *segment = (segment_t*)(jpegBuffer + size);
        uint8_t type = checkJpegMarker(segment->marker);
        if (type == 0) { // invalid marker, no more segments, begin JPEG data
            ALOGV("JPEG stream found beginning at offset %d", size);
            break;
        }
        if (type == EOI || size > maxSize - sizeof(segment_t)) {
            ALOGE("Got premature End before JPEG data, offset %d", size);
            return 0;
        }
        size_t length = ntohs(segment->length);
        ALOGV("JFIF Segment, type %x length %x", type, length);
        size += length + MARKER_LENGTH;
    }

    // Find End of Image
    // Scan JPEG buffer until End of Image (EOI)
    bool foundEnd = false;
    for ( ; size <= maxSize - MARKER_LENGTH; size++) {
        if ( checkJpegEnd(jpegBuffer + size) ) {
            foundEnd = true;
            size += MARKER_LENGTH;
            break;
        }
    }
    if (!foundEnd) {
        ALOGE("Could not find end of JPEG marker");
        return 0;
    }

    if (size > maxSize) {
        ALOGW("JPEG size %d too large, reducing to maxSize %d", size, maxSize);
        size = maxSize;
    }
    ALOGV("Final JPEG size %d", size);
    return size;
}

}; // namespace camera2
}; // namespace android
