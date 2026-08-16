#import <AVFoundation/AVFoundation.h>
#import <AppKit/AppKit.h>
#import <Foundation/Foundation.h>
#import <ScreenCaptureKit/ScreenCaptureKit.h>
#include <errno.h>
#include <signal.h>
#include <unistd.h>

@interface RecordingDelegate : NSObject <SCRecordingOutputDelegate>
@property(nonatomic, strong) NSError *failure;
@property(nonatomic) BOOL started;
@property(nonatomic) BOOL finished;
@end

@implementation RecordingDelegate
- (void)recordingOutputDidStartRecording:(SCRecordingOutput *)recordingOutput
{
    self.started = YES;
}
- (void)recordingOutput:(SCRecordingOutput *)recordingOutput
       didFailWithError:(NSError *)error
{
    self.failure = error;
    self.finished = YES;
}
- (void)recordingOutputDidFinishRecording:(SCRecordingOutput *)recordingOutput
{
    self.finished = YES;
}
@end

static BOOL ProcessExists(pid_t pid)
{
    return kill(pid, 0) == 0 || errno == EPERM;
}

static SCShareableContent *GetShareableContent(NSError **outError)
{
    __block SCShareableContent *result = nil;
    __block NSError *failure = nil;
    dispatch_semaphore_t done = dispatch_semaphore_create(0);
    [SCShareableContent getShareableContentExcludingDesktopWindows:YES
                                               onScreenWindowsOnly:YES
                                                 completionHandler:^(SCShareableContent *content, NSError *error) {
        result = content;
        failure = error;
        dispatch_semaphore_signal(done);
    }];
    dispatch_semaphore_wait(done, DISPATCH_TIME_FOREVER);
    if (outError)
        *outError = failure;
    return result;
}

static SCWindow *FindWindow(pid_t pid, SCDisplay **outDisplay, NSError **outError)
{
    NSDate *deadline = [NSDate dateWithTimeIntervalSinceNow:15.0];
    do {
        NSError *error = nil;
        SCShareableContent *content = GetShareableContent(&error);
        if (!content) {
            if (outError)
                *outError = error;
            return nil;
        }

        SCWindow *best = nil;
        CGFloat bestArea = 0.0;
        for (SCWindow *window in content.windows) {
            if (window.owningApplication.processID != pid ||
                window.frame.size.width <= 100.0 ||
                window.frame.size.height <= 100.0)
                continue;
            CGFloat area = window.frame.size.width * window.frame.size.height;
            if (area > bestArea) {
                best = window;
                bestArea = area;
            }
        }
        if (best) {
            SCDisplay *selected = nil;
            for (SCDisplay *display in content.displays) {
                if (CGRectIntersectsRect(display.frame, best.frame)) {
                    selected = display;
                    break;
                }
            }
            if (!selected)
                selected = content.displays.firstObject;
            if (outDisplay)
                *outDisplay = selected;
            return best;
        }
        usleep(100000);
    } while ([deadline timeIntervalSinceNow] > 0.0 && ProcessExists(pid));
    return nil;
}

static void PrintError(NSString *message)
{
    fprintf(stderr, "error: %s\n", message.UTF8String);
}

int main(int argc, const char *argv[])
{
    @autoreleasepool {
        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyProhibited];

        pid_t pid = 0;
        NSString *outputPath = nil;
        for (int index = 1; index < argc; ++index) {
            if (strcmp(argv[index], "--pid") == 0 && index + 1 < argc)
                pid = (pid_t)strtol(argv[++index], NULL, 10);
            else if (strcmp(argv[index], "--output") == 0 && index + 1 < argc)
                outputPath = [NSString stringWithUTF8String:argv[++index]];
            else {
                PrintError(@"usage: ScreenCaptureWindow --pid PID --output FILE.mov");
                return 2;
            }
        }
        if (pid <= 0 || outputPath.length == 0) {
            PrintError(@"usage: ScreenCaptureWindow --pid PID --output FILE.mov");
            return 2;
        }

        __block NSError *error = nil;
        SCDisplay *display = nil;
        SCWindow *window = FindWindow(pid, &display, &error);
        if (!window) {
            PrintError(error.localizedDescription ?: [NSString stringWithFormat:
                @"no capturable on-screen window appeared for PID %d", pid]);
            return 4;
        }

        CGFloat scale = 1.0;
        if (display && display.frame.size.width > 0.0)
            scale = MAX(1.0, (CGFloat)display.width / display.frame.size.width);
        size_t width = MAX(2, (size_t)llround(window.frame.size.width * scale));
        size_t height = MAX(2, (size_t)llround(window.frame.size.height * scale));
        width &= ~(size_t)1;
        height &= ~(size_t)1;

        SCStreamConfiguration *configuration = [[SCStreamConfiguration alloc] init];
        configuration.width = width;
        configuration.height = height;
        configuration.minimumFrameInterval = CMTimeMake(1, 30);
        configuration.queueDepth = 5;
        configuration.showsCursor = NO;
        configuration.capturesAudio = NO;

        NSURL *outputURL = [NSURL fileURLWithPath:outputPath];
        [[NSFileManager defaultManager] removeItemAtURL:outputURL error:nil];
        SCRecordingOutputConfiguration *recordingConfiguration =
            [[SCRecordingOutputConfiguration alloc] init];
        recordingConfiguration.outputURL = outputURL;
        recordingConfiguration.videoCodecType = AVVideoCodecTypeH264;
        recordingConfiguration.outputFileType = AVFileTypeQuickTimeMovie;

        RecordingDelegate *delegate = [[RecordingDelegate alloc] init];
        SCRecordingOutput *recordingOutput =
            [[SCRecordingOutput alloc] initWithConfiguration:recordingConfiguration
                                                   delegate:delegate];
        SCContentFilter *filter =
            [[SCContentFilter alloc] initWithDesktopIndependentWindow:window];
        SCStream *stream = [[SCStream alloc] initWithFilter:filter
                                             configuration:configuration
                                                  delegate:nil];
        if (![stream addRecordingOutput:recordingOutput error:&error]) {
            PrintError(error.localizedDescription);
            return 5;
        }

        dispatch_semaphore_t started = dispatch_semaphore_create(0);
        [stream startCaptureWithCompletionHandler:^(NSError *startError) {
            error = startError;
            dispatch_semaphore_signal(started);
        }];
        dispatch_semaphore_wait(started, DISPATCH_TIME_FOREVER);
        if (error) {
            PrintError(error.localizedDescription);
            return 6;
        }
        while (ProcessExists(pid) && !delegate.failure)
            usleep(100000);

        __block NSError *stopFailure = nil;
        dispatch_semaphore_t stopped = dispatch_semaphore_create(0);
        [stream stopCaptureWithCompletionHandler:^(NSError *stopError) {
            stopFailure = stopError;
            dispatch_semaphore_signal(stopped);
        }];
        dispatch_semaphore_wait(stopped, DISPATCH_TIME_FOREVER);

        NSDate *finishDeadline = [NSDate dateWithTimeIntervalSinceNow:10.0];
        while (!delegate.finished && [finishDeadline timeIntervalSinceNow] > 0.0)
            usleep(50000);
        if (delegate.failure)
            error = delegate.failure;
        BOOL hasOutput = [[NSFileManager defaultManager] fileExistsAtPath:outputPath];
        if (!hasOutput) {
            PrintError((error ?: stopFailure).localizedDescription ?:
                       @"ScreenCaptureKit produced no video file");
            return 8;
        }
        if (error) {
            PrintError(error.localizedDescription);
            return 7;
        }
        printf("captured %zux%zu window video\n", width, height);
        return 0;
    }
}
