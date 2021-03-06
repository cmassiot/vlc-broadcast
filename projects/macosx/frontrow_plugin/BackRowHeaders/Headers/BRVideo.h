/*
 *     Generated by class-dump 3.1.1.
 *
 *     class-dump is Copyright (C) 1997-1998, 2000-2001, 2004-2006 by Steve Nygard.
 */

#import "NSObject.h"

@class BRVideoLoadMonitor, NSArray, QTMovie;

@interface BRVideo : NSObject
{
    struct OpaqueQTVisualContext *_textureContext;
    struct __CVBuffer *_currentFrame;
    BRVideoLoadMonitor *_loadMonitor;
    struct CGSize _contextSizeHint;
    id <BRMediaAsset> _media;
    struct TrackType **_movieTrack;
    QTMovie *_movie;
    float _rate;
    double _initialAudioDeviceSampleRate;
    long _chapterTimeScale;
    NSArray *_chapters;
    double _prevScanTime;
    double _movieScanPosition;
    double _timeFreq;
    struct BRVideoTimeRange _bufferedRange;
    float _bufferingProgress;
    double _movieDuration;
    double _movieTime;
    long _cachedChapterIndex;
    BOOL _captionsEnabled;
    struct BRVideoPlaybackStats _stats;
    BOOL _logStalls;
    BOOL _gatherStats;
    BOOL _loops;
    BOOL _muted;
}

- (id)init;
- (id)initWithMedia:(id)fp8 attributes:(id)fp12 error:(id *)fp16;
- (id)media;
- (void)dealloc;
- (void)setPlaybackContext:(id)fp8;
- (void)skip:(double)fp8;
- (void)setElapsedTime:(double)fp8;
- (double)elapsedTime;
- (double)duration;
- (float)aspectRatio;
- (float)bufferingProgress;
- (struct BRVideoTimeRange)bufferedRange;
- (BOOL)videoPlayable;
- (void)gotoBeginning;
- (BOOL)newFrameForTime:(const CDAnonymousStruct2 *)fp8 frame:(struct __CVBuffer **)fp12;
- (struct __CVBuffer *)currentFrame;
- (void)setContextSize:(struct CGSize)fp8;
- (void)setMuted:(BOOL)fp8;
- (BOOL)muted;
- (void)setLoops:(BOOL)fp8;
- (BOOL)loops;
- (void)setCaptionsEnabled:(BOOL)fp8;
- (BOOL)captionsEnabled;
- (void)setRate:(float)fp8;
- (float)rate;
- (id)chapterList;
- (long)currentChapterIndex;
- (void)setGatherPlaybackStats:(BOOL)fp8;
- (struct BRVideoPlaybackStats)playbackStats;

@end

