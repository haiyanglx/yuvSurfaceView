/*
 * Copyright 2013 The Android Open Source Project
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

#include <assert.h>
#include <ctype.h>
#include <fcntl.h>
#include <inttypes.h>
#include <getopt.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>

#include <termios.h>
#include <unistd.h>

#define LOG_TAG "MyShowYUV"
#define ATRACE_TAG ATRACE_TAG_GRAPHICS
//#define LOG_NDEBUG 0
#include <utils/Log.h>

#include <binder/IPCThreadState.h>
#include <utils/Errors.h>
#include <utils/Timers.h>
#include <utils/Trace.h>

#include <gui/Surface.h>
#include <gui/SurfaceComposerClient.h>
#include <gui/ISurfaceComposer.h>
#include <ui/DisplayInfo.h>
#include <ui/GraphicBufferMapper.h>
#include <system/window.h>
#include <ui/GraphicBufferMapper.h>
#include <gui/IGraphicBufferProducer.h>
#include <media/openmax/OMX_IVCommon.h>
#include <media/stagefright/foundation/ABuffer.h>
#include <media/stagefright/foundation/AMessage.h>
#include <media/stagefright/MediaCodec.h>
#include <media/stagefright/MediaErrors.h>
#include <media/stagefright/MediaMuxer.h>
#include <media/ICrypto.h>

#include "OMX_Core.h"

#include "screenrecord.h"
#include "Overlay.h"
#include "FrameOutput.h"

using namespace android;

static const uint32_t kMinBitRate = 100000;         // 0.1Mbps
static const uint32_t kMaxBitRate = 200 * 1000000;  // 200Mbps
static const uint32_t kMaxTimeLimitSec = 180;       // 3 minutes
static const uint32_t kFallbackWidth = 1280;        // 720p
static const uint32_t kFallbackHeight = 720;
static const char* kMimeTypeAvc = "video/avc";

// Command-line parameters.
static bool gVerbose = false;           // chatty on stdout
static bool gRotate = false;            // rotate 90 degrees
static enum {
    FORMAT_MP4, FORMAT_H264, FORMAT_FRAMES, FORMAT_RAW_FRAMES
} gOutputFormat = FORMAT_MP4;           // data format for output
static bool gSizeSpecified = false;     // was size explicitly requested?
static bool gWantInfoScreen = false;    // do we want initial info screen?
static bool gWantFrameTime = false;     // do we want times on each frame?
static uint32_t gVideoWidth = 0;        // default width+height
static uint32_t gVideoHeight = 0;
static uint32_t gBitRate = 4000000;     // 4Mbps
static uint32_t gTimeLimitSec = kMaxTimeLimitSec;

// Set by signal handler to stop recording.
static volatile bool gStopRequested = false;

// Previous signal handler state, restored after first hit.
static struct sigaction gOrigSigactionINT;
static struct sigaction gOrigSigactionHUP;

ANativeWindowBuffer *winbuf;


/*
 * Catch keyboard interrupt signals.  On receipt, the "stop requested"
 * flag is raised, and the original handler is restored (so that, if
 * we get stuck finishing, a second Ctrl-C will kill the process).
 */
static void signalCatcher(int signum)
{
    gStopRequested = true;
    switch (signum) {
    case SIGINT:
    case SIGHUP:
        sigaction(SIGINT, &gOrigSigactionINT, NULL);
        sigaction(SIGHUP, &gOrigSigactionHUP, NULL);
        break;
    default:
        abort();
        break;
    }
}

/*
 * Configures signal handlers.  The previous handlers are saved.
 *
 * If the command is run from an interactive adb shell, we get SIGINT
 * when Ctrl-C is hit.  If we're run from the host, the local adb process
 * gets the signal, and we get a SIGHUP when the terminal disconnects.
 */
static status_t configureSignals() {
    struct sigaction act;
    memset(&act, 0, sizeof(act));
    act.sa_handler = signalCatcher;
    if (sigaction(SIGINT, &act, &gOrigSigactionINT) != 0) {
        status_t err = -errno;
        fprintf(stderr, "Unable to configure SIGINT handler: %s\n",
                strerror(errno));
        return err;
    }
    if (sigaction(SIGHUP, &act, &gOrigSigactionHUP) != 0) {
        status_t err = -errno;
        fprintf(stderr, "Unable to configure SIGHUP handler: %s\n",
                strerror(errno));
        return err;
    }
    return NO_ERROR;
}

/*
 * Returns "true" if the device is rotated 90 degrees.
 */
static bool isDeviceRotated(int orientation) {
    return orientation != DISPLAY_ORIENTATION_0 &&
            orientation != DISPLAY_ORIENTATION_180;
}


static int ALIGN(int x, int y) {
    // y must be a power of 2.
    return (x + y - 1) & ~(y - 1);
}

static OMX_U32 GetGrallocFormat(OMX_U32 nFormat) {

    switch (nFormat) {
        case OMX_COLOR_FormatYUV420Planar:
              /* Cr Cb will be swapped, mdp limitation
               * can be fixed in color convertors if req.
               * This will require Input width and height to
               * be already aligned to hw requirements.
               */
              return HAL_PIXEL_FORMAT_YV12;
        case OMX_COLOR_Format32BitRGBA8888:
            return HAL_PIXEL_FORMAT_RGBA_8888;
        default:
            return nFormat;
    }
}


static void prepareRender(const sp<ANativeWindow> &nativeWindow,int width,int height) {
			
    sp<ANativeWindow> m_pNativeWindow = nativeWindow;
    //int err;
	int mCropWidth = width;
	int mCropHeight = height;
	
	int m_nColorFormat = HAL_PIXEL_FORMAT_YV12;//颜色空间
    int bufWidth = (mCropWidth + 1) & ~1;//按2对齐
    int bufHeight = (mCropHeight + 1) & ~1;
	
	int m_nFrameWidth = width;
	int m_nFrameHeight = height;
	
	native_window_api_connect(m_pNativeWindow.get(), NATIVE_WINDOW_API_MEDIA);
	
	status_t err = native_window_set_scaling_mode(
            m_pNativeWindow.get(), NATIVE_WINDOW_SCALING_MODE_SCALE_TO_WINDOW);
    if (err != OK) {
        printf("native_window_set_scaling_mode failed");
        return;
    }
	
	android_native_rect_t crop;
    crop.left = 0;
    crop.top = 0;
    crop.right = m_nFrameWidth;
    crop.bottom = m_nFrameHeight;

    printf("nativeWindow set crop: [%d, %d] [%d, %d]",
            crop.left, crop.top, crop.right, crop.bottom);
    err = native_window_set_crop(m_pNativeWindow.get(), &crop);
    if (err != OK) {
        printf("native_window_set_crop failed");
        return;
    }

    printf("nativeWindow set geometry: w=%u h=%u",
            (unsigned int)m_nFrameWidth, (unsigned int)m_nFrameHeight);
			
    err = native_window_set_buffers_geometry(m_pNativeWindow.get(),
            m_nFrameWidth, m_nFrameHeight, GetGrallocFormat(m_nColorFormat));
    if (err != OK) {
        printf("native_window_set_buffers_geometry failed");
        return;
    }
	
	err = native_window_set_usage(m_pNativeWindow.get(), GRALLOC_USAGE_SW_READ_NEVER | GRALLOC_USAGE_SW_WRITE_OFTEN |
            GRALLOC_USAGE_HW_TEXTURE | GRALLOC_USAGE_EXTERNAL_DISP);
    if (err != 0) {
        printf("native_window_set_usage failed: %s (%d)",
                strerror(-err), -err);
        return;
    }
	
	printf("Surface  render start \n");	
	
}



static void render(const void *data, size_t size, const sp<ANativeWindow> &nativeWindow,int width,int height) {
			
    sp<ANativeWindow> m_pNativeWindow = nativeWindow;
    int err;
	int mCropWidth = width;
	int mCropHeight = height;
	
	int m_nColorFormat = HAL_PIXEL_FORMAT_YV12;//颜色空间
    int bufWidth = (mCropWidth + 1) & ~1;//按2对齐
    int bufHeight = (mCropHeight + 1) & ~1;
	
	int m_nFrameWidth = width;
	int m_nFrameHeight = height;
	
	/*
	
	native_window_api_connect(m_pNativeWindow.get(), NATIVE_WINDOW_API_MEDIA);
	
	
	status_t err = native_window_set_scaling_mode(
            m_pNativeWindow.get(), NATIVE_WINDOW_SCALING_MODE_SCALE_TO_WINDOW);
    if (err != OK) {
        printf("native_window_set_scaling_mode failed");
        return;
    }
	
	android_native_rect_t crop;
    crop.left = 0;
    crop.top = 0;
    crop.right = m_nFrameWidth;
    crop.bottom = m_nFrameHeight;

    printf("nativeWindow set crop: [%d, %d] [%d, %d]",
            crop.left, crop.top, crop.right, crop.bottom);
    err = native_window_set_crop(m_pNativeWindow.get(), &crop);
    if (err != OK) {
        printf("native_window_set_crop failed");
        return;
    }

    printf("nativeWindow set geometry: w=%u h=%u",
            (unsigned int)m_nFrameWidth, (unsigned int)m_nFrameHeight);
			
    err = native_window_set_buffers_geometry(m_pNativeWindow.get(),
            m_nFrameWidth, m_nFrameHeight, GetGrallocFormat(m_nColorFormat));
    if (err != OK) {
        printf("native_window_set_buffers_geometry failed");
        return;
    }
	
	err = native_window_set_usage(m_pNativeWindow.get(), GRALLOC_USAGE_SW_READ_NEVER | GRALLOC_USAGE_SW_WRITE_OFTEN |
            GRALLOC_USAGE_HW_TEXTURE | GRALLOC_USAGE_EXTERNAL_DISP);
    if (err != 0) {
        printf("native_window_set_usage failed: %s (%d)",
                strerror(-err), -err);
        return;
    }

	*/
	
	
	
	
    err = native_window_dequeue_buffer_and_wait(m_pNativeWindow.get(), &winbuf);
    if(err != 0) {
        printf("dequeueBuffer failed: %s (%d)",strerror(-err), -err);
        return; //TBD: cancel allocated ones
    }
	
	printf("Surface  get native window sucess");
 
    GraphicBufferMapper &mapper = GraphicBufferMapper::get();
 
    Rect bounds(mCropWidth, mCropHeight);
 
    void *dst;
	
    CHECK_EQ(0, mapper.lock(//用来锁定一个图形缓冲区并将缓冲区映射到用户进程
                winbuf->handle, GRALLOC_USAGE_SW_WRITE_OFTEN, bounds, &dst));//dst就指向图形缓冲区首地址
 
    if (true){
        size_t dst_y_size = winbuf->stride * winbuf->height;
        size_t dst_c_stride = ALIGN(winbuf->stride / 2, 16);//1行v/u的大小
        size_t dst_c_size = dst_c_stride * winbuf->height / 2;//u/v的大小
        
        memcpy(dst, data, dst_y_size + dst_c_size*2);//将yuv数据copy到图形缓冲区
    }
 
    CHECK_EQ(0, mapper.unlock(winbuf->handle));
	
	sleep(1);
 
    if ((err = m_pNativeWindow->queueBuffer(m_pNativeWindow.get(), winbuf,
            -1)) != 0) {
        printf("Surface::queueBuffer returned error %d", err);
    }
	
	printf("Surface::queueBuffer over");
	
	
	
	
	
    //winbuf = NULL;
}

static bool getYV12Data(const char *path,unsigned char * pYUVData,int size){
	FILE *fp = fopen(path,"rb");
	if(fp == NULL){
		printf("read %s fail !!!!!!!!!!!!!!!!!!!\n",path);
		return false;
	}
	fread(pYUVData,size,1,fp);
	fclose(fp);
	return true;
}


static void destroySurface(const sp<ANativeWindow> &nativeWindow,sp<Surface> &surface,sp<SurfaceControl> &Control,sp<SurfaceControl> &BackgroundControl) {

	sp<ANativeWindow> m_pNativeWindow = nativeWindow;
	sp<Surface> m_pSurface = surface;
	sp<SurfaceControl> m_pControl = Control;
	sp<SurfaceControl> m_pBackgroundControl = BackgroundControl;
	
    if (m_pNativeWindow.get() != NULL) {
        native_window_api_disconnect(m_pNativeWindow.get(), NATIVE_WINDOW_API_MEDIA);
        m_pNativeWindow.clear();
        m_pNativeWindow = NULL;
    }

    if (m_pSurface.get() != NULL) {
        m_pSurface.clear();
        m_pSurface = NULL;
    }

    if (m_pControl.get() != NULL) {
        m_pControl.clear();
        m_pControl = NULL;
    }

    if (m_pBackgroundControl.get() != NULL) {
        m_pBackgroundControl.clear();
        m_pBackgroundControl = NULL;
    }
    
    printf("I'm getting free!!");
}


/*
 * Main "do work" start point.
 *
 * Configures codec, muxer, and virtual display, then starts moving bits
 * around.
 */
static status_t showYUV(const char* fileName) {
    status_t err;

    // Configure signal handler.
    err = configureSignals();
    if (err != NO_ERROR) return err;

    // Start Binder thread pool.  MediaCodec needs to be able to receive
    // messages from mediaserver.
    sp<ProcessState> self = ProcessState::self();
    self->startThreadPool();
	
	sp<SurfaceComposerClient> client = new SurfaceComposerClient();

    // Get main display parameters.
    sp<IBinder> mainDpy = SurfaceComposerClient::getBuiltInDisplay(
            ISurfaceComposer::eDisplayIdMain);
			
    DisplayInfo mainDpyInfo;
    err = SurfaceComposerClient::getDisplayInfo(mainDpy, &mainDpyInfo);
    if (err != NO_ERROR) {
        fprintf(stderr, "ERROR: unable to get display characteristics\n");
        return err;
    }
	
    if (true) {
        printf("Main display is %dx%d @%.2ffps (orientation=%u)\n",
                mainDpyInfo.w, mainDpyInfo.h, mainDpyInfo.fps,
                mainDpyInfo.orientation);
    }
			
	sp<SurfaceControl> m_pBackgroundControl = client->createSurface(String8("vdec-bkgnd"), mainDpyInfo.w,mainDpyInfo.h, PIXEL_FORMAT_RGB_565);

    sp<SurfaceControl> m_pControl = client->createSurface(String8("vdec-surface"), mainDpyInfo.w,mainDpyInfo.h, PIXEL_FORMAT_OPAQUE);
	
	int width,height;
	width = 240;
	height = 320;
	int size = width * height * 3/2;
	unsigned char *data = new unsigned char[size];
	
	//getYV12Data(fileName,data,size);//get yuv data from file;


    /*********************配置surface*******************************************************************/
    SurfaceComposerClient::openGlobalTransaction();
	
	if (m_pControl->setLayer(100000 + 1) != (status_t)OK) {
            printf("m_pControl->setLayer failed");
    }
	
	if (m_pBackgroundControl->setLayer(100000) != (status_t)OK) {
        printf("m_pBackgroundControl->setLayer failed");
    }
	
	m_pBackgroundControl->show();
	
    //surfaceControl->setLayer(100000);//设定Z坐标
	m_pControl->setPosition(0, 0);//以左上角为(0,0)设定显示位置
	m_pControl->setSize(width, height);//设定视频显示大小
    SurfaceComposerClient::closeGlobalTransaction();
	sp<Surface> surface = m_pControl->getSurface();
	printf("[%s][%d]\n",__FILE__,__LINE__);
	
	
	
/**********************显示yuv数据******************************************************************/	

	prepareRender(surface,width,height);
	
	FILE *fp = fopen(fileName,"rb");
	if(fp == NULL){
		printf("read %s fail !!!!!!!!!!!!!!!!!!!\n",fileName);
		return false;
	}
	while(!feof(fp)){
		int num  = fread(data,1,size,fp);
		printf("read data [%d]\n",num);
		render(data,size,surface,width,height);
	}
	fclose(fp);
	
	sp<ANativeWindow> m_pNativeWindow = surface;
	
	err = m_pNativeWindow->cancelBuffer(m_pNativeWindow.get(), winbuf, -1);
    if (err != 0) {
        printf("cancelBuffer failed w/ error 0x%08x", err);
        return err;
    }
	
	destroySurface(surface,surface,m_pControl,m_pBackgroundControl);
	
	printf("[%s][%d]\n",__FILE__,__LINE__);
	
	client->dispose();
	
	IPCThreadState::self()->joinThreadPool();//可以保证画面一直显示，否则瞬间消失
    IPCThreadState::self()->stopProcess();
    

    return err;
}




/*
 * Sends a broadcast to the media scanner to tell it about the new video.
 *
 * This is optional, but nice to have.
 */
static status_t notifyMediaScanner(const char* fileName) {
    // need to do allocations before the fork()
    String8 fileUrl("file://");
    fileUrl.append(fileName);

    const char* kCommand = "/system/bin/am";
    const char* const argv[] = {
            kCommand,
            "broadcast",
            "-a",
            "android.intent.action.MEDIA_SCANNER_SCAN_FILE",
            "-d",
            fileUrl.string(),
            NULL
    };
    if (gVerbose) {
        printf("Executing:");
        for (int i = 0; argv[i] != NULL; i++) {
            printf(" %s", argv[i]);
        }
        putchar('\n');
    }

    pid_t pid = fork();
    if (pid < 0) {
        int err = errno;
        ALOGW("fork() failed: %s", strerror(err));
        return -err;
    } else if (pid > 0) {
        // parent; wait for the child, mostly to make the verbose-mode output
        // look right, but also to check for and log failures
        int status;
        pid_t actualPid = TEMP_FAILURE_RETRY(waitpid(pid, &status, 0));
        if (actualPid != pid) {
            ALOGW("waitpid(%d) returned %d (errno=%d)", pid, actualPid, errno);
        } else if (status != 0) {
            ALOGW("'am broadcast' exited with status=%d", status);
        } else {
            ALOGV("'am broadcast' exited successfully");
        }
    } else {
        if (!gVerbose) {
            // non-verbose, suppress 'am' output
            ALOGV("closing stdout/stderr in child");
            int fd = open("/dev/null", O_WRONLY);
            if (fd >= 0) {
                dup2(fd, STDOUT_FILENO);
                dup2(fd, STDERR_FILENO);
                close(fd);
            }
        }
        execv(kCommand, const_cast<char* const*>(argv));
        ALOGE("execv(%s) failed: %s\n", kCommand, strerror(errno));
        exit(1);
    }
    return NO_ERROR;
}

/*
 * Parses a string of the form "1280x720".
 *
 * Returns true on success.
 */
static bool parseWidthHeight(const char* widthHeight, uint32_t* pWidth,
        uint32_t* pHeight) {
    long width, height;
    char* end;

    // Must specify base 10, or "0x0" gets parsed differently.
    width = strtol(widthHeight, &end, 10);
    if (end == widthHeight || *end != 'x' || *(end+1) == '\0') {
        // invalid chars in width, or missing 'x', or missing height
        return false;
    }
    height = strtol(end + 1, &end, 10);
    if (*end != '\0') {
        // invalid chars in height
        return false;
    }

    *pWidth = width;
    *pHeight = height;
    return true;
}

/*
 * Accepts a string with a bare number ("4000000") or with a single-character
 * unit ("4m").
 *
 * Returns an error if parsing fails.
 */
static status_t parseValueWithUnit(const char* str, uint32_t* pValue) {
    long value;
    char* endptr;

    value = strtol(str, &endptr, 10);
    if (*endptr == '\0') {
        // bare number
        *pValue = value;
        return NO_ERROR;
    } else if (toupper(*endptr) == 'M' && *(endptr+1) == '\0') {
        *pValue = value * 1000000;  // check for overflow?
        return NO_ERROR;
    } else {
        fprintf(stderr, "Unrecognized value: %s\n", str);
        return UNKNOWN_ERROR;
    }
}

/*
 * Dumps usage on stderr.
 */
static void usage() {
    fprintf(stderr,
        "Usage: screenrecord [options] <filename>\n"
        "\n"
        "Android screenrecord v%d.%d.  Records the device's display to a .mp4 file.\n"
        "\n"
        "Options:\n"
        "--size WIDTHxHEIGHT\n"
        "    Set the video size, e.g. \"1280x720\".  Default is the device's main\n"
        "    display resolution (if supported), 1280x720 if not.  For best results,\n"
        "    use a size supported by the AVC encoder.\n"
        "--bit-rate RATE\n"
        "    Set the video bit rate, in bits per second.  Value may be specified as\n"
        "    bits or megabits, e.g. '4000000' is equivalent to '4M'.  Default %dMbps.\n"
        "--bugreport\n"
        "    Add additional information, such as a timestamp overlay, that is helpful\n"
        "    in videos captured to illustrate bugs.\n"
        "--time-limit TIME\n"
        "    Set the maximum recording time, in seconds.  Default / maximum is %d.\n"
        "--verbose\n"
        "    Display interesting information on stdout.\n"
        "--help\n"
        "    Show this message.\n"
        "\n"
        "Recording continues until Ctrl-C is hit or the time limit is reached.\n"
        "\n",
        kVersionMajor, kVersionMinor, gBitRate / 1000000, gTimeLimitSec
        );
}

/*
 * Parses args and kicks things off.
 */
int main(int argc, char* const argv[]) {
    static const struct option longOptions[] = {
        { "help",               no_argument,        NULL, 'h' },
        { "verbose",            no_argument,        NULL, 'v' },
        { "size",               required_argument,  NULL, 's' },
        { "bit-rate",           required_argument,  NULL, 'b' },
        { "time-limit",         required_argument,  NULL, 't' },
        { "bugreport",          no_argument,        NULL, 'u' },
        // "unofficial" options
        { "show-device-info",   no_argument,        NULL, 'i' },
        { "show-frame-time",    no_argument,        NULL, 'f' },
        { "rotate",             no_argument,        NULL, 'r' },
        { "output-format",      required_argument,  NULL, 'o' },
        { NULL,                 0,                  NULL, 0 }
    };
	
	fprintf(stderr, "argc=%d,optind=%d,file name=%s.\n",argc,optind,argv[argc-1]);

    const char* fileName = argv[argc - 1];
	
    //if (gOutputFormat == FORMAT_MP4) {
        // MediaMuxer tries to create the file in the constructor, but we don't
        // learn about the failure until muxer.start(), which returns a generic
        // error code without logging anything.  We attempt to create the file
        // now for better diagnostics.
        int fd = open(fileName, O_RDWR, 0644);
        if (fd < 0) {
            fprintf(stderr, "Unable to open '%s': %s\n", fileName, strerror(errno));
            return 1;
        }
        close(fd);
    //}

    status_t err = showYUV(fileName);
    if (err == NO_ERROR) {
        // Try to notify the media scanner.  Not fatal if this fails.
        //notifyMediaScanner(fileName);
		
    }
	
    ALOGD(err == NO_ERROR ? "success" : "failed");
    return (int) err;
}
