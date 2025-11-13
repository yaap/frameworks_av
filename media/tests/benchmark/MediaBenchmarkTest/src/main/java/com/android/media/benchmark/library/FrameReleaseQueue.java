/*
 * Copyright (C) 2022 The Android Open Source Project
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

package com.android.media.benchmark.library;

import android.media.MediaCodec;
import android.util.Log;
import androidx.annotation.NonNull;
import java.nio.ByteBuffer;
import java.util.concurrent.LinkedBlockingQueue;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.ScheduledExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.TimeUnit;

public class FrameReleaseQueue {
    private static final String TAG = "FrameReleaseQueue";
    private static final boolean DEBUG = false;
    private final String MIME_AV1 = "video/av01";
    private final int AV1_SUPERFRAME_DELAY = 6;
    private final int THRESHOLD_TIME = 5;

    private final long HOUR_IN_MS = (60 * 60 * 1000L);
    private final long MINUTE_IN_MS = (60 * 1000L);

    private MediaCodec mCodec;
    private LinkedBlockingQueue<FrameInfo> mFrameInfoQueue;
    private ReleaseThread mReleaseThread;
    private AtomicBoolean doFrameRelease = new AtomicBoolean(false);
    private AtomicBoolean mReleaseJobStarted = new AtomicBoolean(false);
    private boolean mRender = false;
    private long mWaitTime = 40; // milliseconds per frame
    private int mWaitTimeCorrection = 0;
    private int mCorrectionLoopCount;
    protected long firstReleaseTime = -1;
    private long mAllowedDelayTime = THRESHOLD_TIME;
    private int mFrameDelay = 0;
    private final ScheduledExecutorService mScheduler = Executors.newScheduledThreadPool(1);

    public FrameReleaseQueue(boolean render, int frameRate) {
        this.mFrameInfoQueue = new LinkedBlockingQueue();
        this.mReleaseThread = new ReleaseThread();
        this.doFrameRelease.set(true);
        this.mRender = render;
        this.mWaitTime = 1000 / frameRate; // wait time in milliseconds per frame
        int waitTimeRemainder = 1000 % frameRate;
        int gcd = gcd(frameRate, waitTimeRemainder);
        this.mCorrectionLoopCount = frameRate / gcd;
        this.mWaitTimeCorrection = waitTimeRemainder / gcd;
        Log.i(TAG, "Constructed FrameReleaseQueue with wait time " + this.mWaitTime + " ms");
    }

    public FrameReleaseQueue(boolean render, int sampleRate, int nChannels, int bytesPerChannel) {
        this.mFrameInfoQueue = new LinkedBlockingQueue();
        this.doFrameRelease.set(true);
        this.mRender = render;
        this.mReleaseThread = new AudioRendererThread(sampleRate, nChannels, bytesPerChannel);
    }

    private static class FrameInfo {
        private int number = 0;
        private int bufferId;
        private int displayTime;
        private int bytes;
        private MediaCodec.OutputFrame mOutputFrame = null;
        public FrameInfo(int frameNumber, int frameBufferId, int frameDisplayTime) {
            this.number = frameNumber;
            this.bufferId = frameBufferId;
            this.displayTime = frameDisplayTime;
        }
        public FrameInfo(int frameBufferId, int bytes) {
            this.bufferId = frameBufferId;
            this.bytes = bytes;
        }
        public void setOutputFrame(MediaCodec.OutputFrame frame) {
            mOutputFrame = frame;
        }
    }

    private class ReleaseThread extends Thread {
        private int mLoopCount = 0;
        private long mNextReleaseTime = 0;

        protected void printPlaybackTime() {
            if (firstReleaseTime == -1) {
                Log.d(TAG, "Playback Time not initialized");
                return;
            }
            long curTime = getCurSysTime() - firstReleaseTime;
            long hours = curTime / (HOUR_IN_MS);
            curTime -= (hours * HOUR_IN_MS);
            long min = curTime / MINUTE_IN_MS;
            curTime -= (min * MINUTE_IN_MS);
            Log.d(TAG, "Playback time: "
                    + hours + "h "
                    + min + "m "
                    + (double)(curTime / (double)1000) +"s");
        }

        @SuppressWarnings("FutureReturnValueIgnored")
        public void run() {
            /* Check if the release thread wakes up too late */
            if (mLoopCount != 0) {
                long delta = getCurSysTime() - mNextReleaseTime;
                if (delta >= THRESHOLD_TIME) {
                    Log.d(TAG, "Release thread wake up late by " + delta);
                    /* For accidental late wake up, we should relax the timestamp
                       check for display time */
                    mAllowedDelayTime = 1 + delta;
                } else {
                    mAllowedDelayTime = THRESHOLD_TIME;
                }
            }
            if (doFrameRelease.get() || mFrameInfoQueue.size() > 0) {
                FrameInfo curFrameInfo = mFrameInfoQueue.peek();
                if (curFrameInfo == null) {
                    mNextReleaseTime += mWaitTime;
                } else {
                    if (firstReleaseTime == -1 || curFrameInfo.displayTime <= 0) {
                        // first frame of loop
                        firstReleaseTime = getCurSysTime();
                        mNextReleaseTime = firstReleaseTime + mWaitTime;
                        popAndRelease(true);
                    } else if (!doFrameRelease.get() && mFrameInfoQueue.size() == 1) {
                        // EOS
                        Log.i(TAG, "EOS");
                        popAndRelease(false);
                    } else {
                        mNextReleaseTime += mWaitTime;
                        long curSysTime = getCurSysTime();
                        long curMediaTime = curSysTime - firstReleaseTime;
                        while (curFrameInfo != null && curFrameInfo.displayTime > 0 &&
                                curFrameInfo.displayTime <= curMediaTime) {
                            if (!((curMediaTime - curFrameInfo.displayTime) <= mAllowedDelayTime)) {
                                Log.d(TAG, "Dropping expired frame " + curFrameInfo.number +
                                    " display time " + curFrameInfo.displayTime +
                                    " current time " + curMediaTime);
                                popAndRelease(false);
                            } else {
                                popAndRelease(true);
                            }
                            curFrameInfo = mFrameInfoQueue.peek();
                        }
                        if (curFrameInfo != null && curFrameInfo.displayTime > curMediaTime) {
                            if ((curFrameInfo.displayTime - curMediaTime) < THRESHOLD_TIME) {
                                // release the frame now as we are already there
                                popAndRelease(true);
                            }
                        }
                    }
                }

                long sleepTime = (long)(mNextReleaseTime - getCurSysTime());
                mScheduler.schedule(mReleaseThread, sleepTime, TimeUnit.MILLISECONDS);

                if (mLoopCount % mCorrectionLoopCount == 0) {
                    mNextReleaseTime += mWaitTimeCorrection;
                }
                mLoopCount += 1;
            } else {
                mReleaseJobStarted.set(false);
            }
        }
    }

    private class AudioRendererThread extends ReleaseThread {
        private final int WAIT_FOR_BUFFER_IN_SEC = 2;
        private double mTimeAdjustMs = 0;
        private double mMsForByte = 0;
        private double mExpectedWakeUpTime = 0;
        private FrameInfo mCurrentFrameInfo;

        AudioRendererThread(int sampleRate, int nChannels, int bytesPerChannel) {
            if (DEBUG) {
                Log.d(TAG, "sampleRate " + sampleRate
                        + " nChannels " + nChannels
                        + " bytesPerChannel " + bytesPerChannel);
            }
            this.mMsForByte = 1000 / (double)(sampleRate * nChannels * bytesPerChannel);
        }

        @Override
        @SuppressWarnings("FutureReturnValueIgnored")
        public void run() {
            long curTime = getCurSysTime();
            if (DEBUG) {
                if (firstReleaseTime == -1) {
                    firstReleaseTime = curTime;
                }
                printPlaybackTime();
            }
            if (mMsForByte == 0) {
                Log.e(TAG, "Audio rendering not possible, no valid params");
                return;
            }
            if (mCurrentFrameInfo != null) {
                try {
                    if (mCurrentFrameInfo.mOutputFrame != null) {
                        try {
                            if (mCurrentFrameInfo.mOutputFrame.getLinearBlock() != null) {
                                mCurrentFrameInfo.mOutputFrame.getLinearBlock().recycle();
                            }
                        } catch (IllegalStateException e) {
                            Log.d(TAG, "Block model buffer recycle error " + e.toString());
                            e.printStackTrace();
                        }
                    }
                    mCodec.releaseOutputBuffer(mCurrentFrameInfo.bufferId, false);
                } catch (IllegalStateException e) {
                    doFrameRelease.set(false);
                    Log.e(TAG, "Threw InterruptedException on releaseOutputBuffer");
                } finally {
                    mCurrentFrameInfo = null;
                }
            }
            boolean requestedSchedule = false;
            try {
                while (doFrameRelease.get() || mFrameInfoQueue.size() > 0) {
                    mCurrentFrameInfo = mFrameInfoQueue.poll(
                            WAIT_FOR_BUFFER_IN_SEC, TimeUnit.SECONDS);
                    if (mCurrentFrameInfo != null) {
                        mTimeAdjustMs = 0;
                        if (mExpectedWakeUpTime != 0) {
                            mTimeAdjustMs = mExpectedWakeUpTime - getCurSysTime();
                        }
                        double sleepTimeUs =
                                (mMsForByte * mCurrentFrameInfo.bytes + mTimeAdjustMs) * 1000;
                        mExpectedWakeUpTime = getCurSysTime() + (sleepTimeUs / 1000);
                        if (DEBUG) {
                            Log.d(TAG, " mExpectedWakeUpTime " + mExpectedWakeUpTime
                                + " Waiting for " + (long)(sleepTimeUs) + "us"
                                + " Now " + getCurSysTime()
                                + " bytes " + mCurrentFrameInfo.bytes
                                + " bufferID " + mCurrentFrameInfo.bufferId);
                        }
                        mScheduler.schedule(
                                mReleaseThread,(long)(sleepTimeUs),TimeUnit.MICROSECONDS);
                        requestedSchedule = true;
                        break;
                    }
                }
            } catch(InterruptedException e) {
                Log.d(TAG, "Interrupted during poll wait");
                doFrameRelease.set(false);
            }
            if (!requestedSchedule) {
                mReleaseJobStarted.set(false);
            }
        }
    }

    private static int gcd(int a, int b) {
        return b == 0 ? a : gcd(b, a % b);
    }

    public void setMediaCodec(MediaCodec mediaCodec) {
        this.mCodec = mediaCodec;
    }

    public void setMime(String mime) {
        if (mime.equals(MIME_AV1)) {
            mFrameDelay = AV1_SUPERFRAME_DELAY;
        }
    }

    public boolean pushFrame(int frameBufferId, int bytes) {
        FrameInfo info = new FrameInfo(frameBufferId, bytes);
        boolean pushSuccess = mFrameInfoQueue.offer(info);
        if (!pushSuccess) {
            Log.e(TAG, "Failed to push frame with buffer id " + info.bufferId);
            return false;
        }
        if (!mReleaseJobStarted.get()) {
            mScheduler.execute(mReleaseThread);
            mReleaseJobStarted.set(true);
        }
        return true;
    }

    // For Block_Model (audio)
    public boolean pushFrame(int frameBufferId, MediaCodec.OutputFrame outFrame, int bytes) {
        FrameInfo info = new FrameInfo(frameBufferId, bytes);
        info.setOutputFrame(outFrame);
        boolean pushSuccess = mFrameInfoQueue.offer(info);
        if (!pushSuccess) {
            Log.e(TAG, "Failed to push frame with buffer id " + info.bufferId);
            return false;
        }
        if (!mReleaseJobStarted.get()) {
            mScheduler.execute(mReleaseThread);
            mReleaseJobStarted.set(true);
        }
        return true;
    }
    public boolean pushFrame(int frameNumber, int frameBufferId, long frameDisplayTime) {
        int frameDisplayTimeMs = (int)(frameDisplayTime/1000);
        FrameInfo curFrameInfo = new FrameInfo(frameNumber, frameBufferId, frameDisplayTimeMs);
        boolean pushSuccess = mFrameInfoQueue.offer(curFrameInfo);
        if (!pushSuccess) {
            Log.e(TAG, "Failed to push frame with buffer id " + curFrameInfo.bufferId);
            return false;
        }

        if (!mReleaseJobStarted.get() && frameNumber >= mFrameDelay) {
            mScheduler.execute(mReleaseThread);
            mReleaseJobStarted.set(true);
            Log.i(TAG, "Started frame release thread");
        }
        return true;
    }

    private long getCurSysTime() {
        return (long)(System.nanoTime() / 1000000L);
    }

    @SuppressWarnings("FutureReturnValueIgnored")
    private void popAndRelease(boolean renderThisFrame) {
        final boolean actualRender = (renderThisFrame && mRender);
        try {
            final FrameInfo curFrameInfo = mFrameInfoQueue.take();

            CompletableFuture.runAsync(() -> {
                try {
                    if (curFrameInfo.mOutputFrame != null) {
                        try {
                            if (curFrameInfo.mOutputFrame.getLinearBlock() != null) {
                                curFrameInfo.mOutputFrame.getLinearBlock().recycle();
                            }
                        } catch (IllegalStateException e) {
                            // nothing to do
                        }
                    }
                    mCodec.releaseOutputBuffer(curFrameInfo.bufferId, actualRender);
                } catch (IllegalStateException e) {
                    throw(e);
                }
            });

        } catch (InterruptedException e) {
            Log.e(TAG, "Threw InterruptedException on take");
        }
    }

    public void stopFrameRelease() {
        doFrameRelease.set(false);
        while (mReleaseJobStarted.get()) {
            try {
                TimeUnit.SECONDS.sleep(1);
            } catch (InterruptedException e) {
                Log.e(TAG, "Threw InterruptedException on sleep");
            }
        }
    }
}

