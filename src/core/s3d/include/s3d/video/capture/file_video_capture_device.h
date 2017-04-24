// Copyright 2013 The Chromium Authors. All rights reserved.
// Inspired by Chromium video capture interface
// Simplified and stripped from internal base code

#ifndef S3D_VIDEO_CAPTURE_FILE_VIDEO_CAPTURE_DEVICE_H
#define S3D_VIDEO_CAPTURE_FILE_VIDEO_CAPTURE_DEVICE_H

#include "video_capture_device.h"

#include <atomic>
#include <thread>

// todo(hugbed): put in s3d namespace

class VideoFileParser;

class FileVideoCaptureDevice : public VideoCaptureDevice {
 public:
  explicit FileVideoCaptureDevice(std::string filePath);

  gsl::owner<VideoCaptureDevice*> clone() const override;

  ~FileVideoCaptureDevice() override;

  void AllocateAndStart(const VideoCaptureFormat& format,
                        std::unique_ptr<VideoCaptureDevice::Client> client) override;

  void StopAndDeAllocate() override;

 private:
  static std::unique_ptr<VideoFileParser> GetVideoFileParser(const std::string& filePath,
                                                             VideoCaptureFormat* format);

  // runs on capture thread
  void OnAllocateAndStart();
  void OnCaptureTask();

  // belongs to capture thread
  std::unique_ptr<std::thread> captureThread_;
  std::unique_ptr<VideoFileParser> fileParser_;
  std::unique_ptr<Client> client_;
  std::string filePath_;
  std::vector<uint8_t> videoFrame_;
  VideoCaptureFormat captureFormat_;
  std::atomic<bool> stopCaptureFlag_;
};

#endif  // S3D_VIDEO_CAPTURE_FILE_VIDEO_CAPTURE_DEVICE_H
