#include "mainwindow.h"
#include "ui_mainwindow.h"

#include "controllers/imageoperationtogglecontroller.h"
#include "controllers/imageoperationtriggercontroller.h"

#include "rendering/openglwindow.h"
#include "rendering/renderingcontext.h"
#include "rendering/texturemanager.h"
#include "utilities/cv.h"
#include "widgets/disparitysettingsdialog.h"
#include "widgets/featuressettingsdialog.h"
#include "widgets/filtersettingsdialog.h"
#include "worker/stereo_demuxer_factory_qimage.h"
#include "worker/stereo_demuxer_qimage.h"
#include "worker/videosynchronizer.h"

#include <s3d/cv/disparity/disparity_analyzer_stan.h>

#include <QFileDialog>
#include <QMouseEvent>
#include <QtGui/QPainter>

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent),
      m_stereoDemuxer{},
      m_stereoDemuxerFactory{std::make_unique<StereoDemuxerFactoryQImage>()},
      m_disparitySettingsDialog{std::make_unique<DisparitySettingsDialog>(this)},
      m_featuresSettingsDialog{std::make_unique<FeaturesSettingsDialog>(this)},
      m_filterSettingsDialog{std::make_unique<FilterSettingsDialog>(this)},
      ui(new Ui::MainWindow) {
  ui->setupUi(this);

  m_disparitySettingsDialog->setUserSettings(&m_userSettings);
  m_videoSynchronizer = std::make_unique<VideoSynchronizer>();
  m_videoSynchronizer->setStereoVideoFormat(s3d::Stereo3DFormat::AboveBelow);
  m_stereoFormat = s3d::Stereo3DFormat::AboveBelow;

  // todo: group connects in separate functions
  // this function is getting laaarge

  m_analyzer = std::make_unique<s3d::DisparityAnalyzerSTAN>();
  m_analyzer->setMinimumNumberOfInliers(m_analyzerMinNbInliers);

  m_imageOperations = std::make_unique<s3d::image_operation::CameraAlignment>(m_analyzer.get());
  m_imageOperations->drawEpilines.disable();
  m_imageOperationsActionControllers.emplace_back(
    std::make_unique<ImageOperationToggleController>(ui->actionEnableComputations, &m_imageOperations->computeAlignment)
  );
  m_imageOperationsActionControllers.emplace_back(
    std::make_unique<ImageOperationToggleController>(ui->actionEpilines, &m_imageOperations->drawEpilines)
  );
  m_imageOperationsActionControllers.emplace_back(
    std::make_unique<ImageOperationTriggerController>(ui->actionUpdateRectification, &m_imageOperations->updateRectification)
  );
  m_imageOperationsActionControllers.emplace_back(
    std::make_unique<ImageOperationToggleController>(ui->actionAutoUpdateRectification, &m_imageOperations->updateRectification)
  );
  m_imageOperationsActionControllers.emplace_back(
    std::make_unique<ImageOperationToggleController>(ui->actionRectify, &m_imageOperations->rectify)
  );
  m_imageOperationsActionControllers.emplace_back(
    std::make_unique<ImageOperationToggleController>(ui->actionFilter, &m_imageOperations->filterAlignment)
  );
  for (auto && actionController : m_imageOperationsActionControllers) {
    actionController->onActionTriggered();
    connect(actionController.get(), &ImageOperationToggleController::imageOperationTriggered, [this] { computeAndUpdate(); });
  }
  connect(ui->actionResetFilter, &QAction::triggered, [this] {
    m_imageOperations->filterAlignment.resetFilter();
    computeAndUpdate();
  });

  ui->depthWidget->setDisplayRange(m_userSettings.displayParameters.displayRangeMin,
                                   m_userSettings.displayParameters.displayRangeMax);
  ui->depthWidget->setExpectedRange(m_userSettings.displayParameters.expectedRangeMin,
                                    m_userSettings.displayParameters.expectedRangeMax);

  ui->actionInputVideo->setChecked(true);
  ui->actionFormatAboveBelow->setChecked(true);
  updateInputMode();
  updateStereo3DFormat();

  connect(
      m_disparitySettingsDialog.get(), &DisparitySettingsDialog::settingsUpdated, [this](UserSettings userSettings) {
        // keep image width in pixels that is not controlled by the user
        int imageWidthPixels = m_userSettings.viewerContext.imageWidthPixels;
        m_userSettings = std::move(userSettings);
        m_userSettings.viewerContext.imageWidthPixels = imageWidthPixels;

        m_currentContext->entityManager->setUserSettings(&m_userSettings);
        m_currentContext->openGLRenderer->updateScene();

        ui->depthWidget->setDisplayRange(m_userSettings.displayParameters.displayRangeMin,
                                         m_userSettings.displayParameters.displayRangeMax);
        ui->depthWidget->setExpectedRange(m_userSettings.displayParameters.expectedRangeMin,
                                          m_userSettings.displayParameters.expectedRangeMax);

        ui->convergenceIndicator->update();
      });

  connect(ui->actionInputImage, &QAction::triggered, [this] { updateInputMode(); });
  connect(ui->actionInputVideo, &QAction::triggered, [this] { updateInputMode(); });
  connect(ui->actionInputLive, &QAction::triggered, [this] { updateInputMode(); });

  connect(ui->actionFormatSeparateFiles, &QAction::triggered, [this] {
    updateInputMode();
    updateStereo3DFormat();
  });
  connect(ui->actionFormatSideBySide, &QAction::triggered, [this] {
    updateInputMode();
    updateStereo3DFormat();
  });
  connect(ui->actionFormatAboveBelow, &QAction::triggered, [this] {
    updateInputMode();
    updateStereo3DFormat();
  });
  connect(ui->actionFormatHalfResolution, &QAction::triggered, [this] { updateStereo3DFormat(); });

  connect(ui->actionDisparitySettings, &QAction::triggered, [this] { m_disparitySettingsDialog->show(); });
  connect(ui->actionFeaturesSettings, &QAction::triggered, [this] { m_featuresSettingsDialog->show(); });
  connect(ui->actionFilterSettings, &QAction::triggered, [this] { m_filterSettingsDialog->show(); });

  connect(m_featuresSettingsDialog.get(), &FeaturesSettingsDialog::featureDetectorChanged, [this] (int newDetectorIndex) {
    m_analyzer->setMatchFinder(MatchFinderFromIndexFactory::create(newDetectorIndex));
    computeAndUpdate();
  });

  connect(m_featuresSettingsDialog.get(), &FeaturesSettingsDialog::maxNbFeaturesChanged, [this] (int newMaxNbFeatures) {
    m_analyzer->setMaxNumberOfFeatures(newMaxNbFeatures);
    computeAndUpdate();
  });

  connect(m_featuresSettingsDialog.get(), &FeaturesSettingsDialog::imageScalingRatioChanged, [this] (float newRatio) {
    m_imageOperations->scaleImages.setRatio(newRatio);
    computeAndUpdate();
  });

  connect(m_filterSettingsDialog.get(), &FilterSettingsDialog::processVarianceChanged, [this] (const s3d::StanVariance& processVariance) {
    m_imageOperations->filterAlignment.setProcessVariance(processVariance);
  });

  connect(m_filterSettingsDialog.get(), &FilterSettingsDialog::startNoiseMeasure, [this] {
    m_imageOperations->measureAlignmentNoise.reset();
    m_imageOperations->measureAlignmentNoise.enable();
  });

  connect(m_filterSettingsDialog.get(), &FilterSettingsDialog::stopNoiseMeasure, [this] {
    m_imageOperations->measureAlignmentNoise.disable();
    auto variance = m_imageOperations->measureAlignmentNoise.getVariance();
    m_imageOperations->filterAlignment.setMeasureVariance(variance);
  });

  connect(ui->actionOpenLeftImage, &QAction::triggered, [this] {
    requestImageFilename([this](const QString& filename) {
      if (m_videoSynchronizer != nullptr) {
        m_videoSynchronizer->stop();
        ui->videoControls->pause();
      }

      m_imageLeft = QImage(filename);
      m_imageLeftReady = true;

      if (m_imageLeftReady && m_imageRightReady) {
        handleNewImagePair(m_imageLeft, m_imageRight, {});
      }
    });
  });

  connect(ui->actionOpenRightImage, &QAction::triggered, [this] {
    requestImageFilename([this](const QString& filename) {
      if (m_videoSynchronizer != nullptr) {
        m_videoSynchronizer->stop();
        ui->videoControls->pause();
      }

      m_imageRight = QImage(filename);
      m_imageRightReady = true;

      if (m_imageLeftReady && m_imageRightReady) {
        handleNewImagePair(m_imageLeft, m_imageRight, {});
      }
    });
  });

  connect(ui->actionOpenImage, &QAction::triggered, [this] {
    requestImageFilename([this](const QString& filename) {
      QImage image(filename);

      if (m_videoSynchronizer != nullptr) {
        m_videoSynchronizer->stop();
        ui->videoControls->pause();
      }

      m_imageLeft = image;
      demuxImage(m_imageLeft);
    });
  });

  connect(ui->actionOpenLeftVideo, &QAction::triggered, [this] {
    requestVideoFilename([this](const QString& filename) {
      if (m_videoSynchronizer != nullptr) {
        m_videoSynchronizer->setLeftFilename(filename.toUtf8().toStdString());

        // if both files set, load video
        if (m_videoSynchronizer->isVideoReadyToLoad()) {
          ui->videoControls->pause();
          m_videoSynchronizer->loadStereoVideo();
        }
      }
    });
  });

  connect(ui->actionOpenRightVideo, &QAction::triggered, [this] {
    requestVideoFilename([this](const QString& filename) {
      if (m_videoSynchronizer != nullptr) {
        m_videoSynchronizer->setRightFilename(filename.toUtf8().toStdString());

        // if both files set, load video
        if (m_videoSynchronizer->isVideoReadyToLoad()) {
          ui->videoControls->pause();
          m_videoSynchronizer->loadStereoVideo();
        }
      }
    });
  });

  connect(ui->actionOpenVideo, &QAction::triggered, [this] {
    requestVideoFilename([this](const QString& filename) {
      if (m_videoSynchronizer != nullptr) {
        m_videoSynchronizer->setLeftFilename(filename.toUtf8().toStdString());

        if (m_videoSynchronizer->isVideoReadyToLoad()) {
          ui->videoControls->pause();
          m_videoSynchronizer->loadStereoVideo();
        }
      }
    });
  });

  connect(ui->actionAnaglyph, &QAction::triggered, [this] {
    m_currentContext->entityManager->displayModeChanged(EntityManager::DisplayMode::Anaglyph);
    m_currentContext->openGLRenderer->updateScene();
  });

  connect(ui->actionOpacity, &QAction::triggered, [this] {
    m_currentContext->entityManager->displayModeChanged(EntityManager::DisplayMode::Opacity);
    m_currentContext->openGLRenderer->updateScene();
  });

  connect(ui->actionInterlaced, &QAction::triggered, [this] {
    m_currentContext->entityManager->displayModeChanged(EntityManager::DisplayMode::Interlaced);
    m_currentContext->openGLRenderer->updateScene();
  });

  connect(ui->actionSide_by_side, &QAction::triggered, [this] {
    m_currentContext->entityManager->displayModeChanged(EntityManager::DisplayMode::SideBySide);
    m_currentContext->openGLRenderer->updateScene();
  });

  connect(ui->actionLeft, &QAction::triggered, [this] {
    m_currentContext->entityManager->displayModeChanged(EntityManager::DisplayMode::Left);
    m_currentContext->openGLRenderer->updateScene();
  });

  connect(ui->actionRight, &QAction::triggered, [this] {
    m_currentContext->entityManager->displayModeChanged(EntityManager::DisplayMode::Right);
    m_currentContext->openGLRenderer->updateScene();
  });

  connect(ui->actionViewer, &QAction::triggered, [this] {
    m_currentContext->entityManager->displayModeChanged(EntityManager::DisplayMode::ViewerCentric);
    m_currentContext->openGLRenderer->updateScene();
  });

  connect(ui->actionFeatures,
          &QAction::triggered,  //
          [this] {
            m_currentContext->entityManager->setFeaturesVisibility(ui->actionFeatures->isChecked());
            m_currentContext->openGLRenderer->updateScene();
          });

  connect(ui->hitWidget,
          static_cast<void (QDoubleSpinBox::*)(double)>(&QDoubleSpinBox::valueChanged),
          [this](double value) {
            m_currentContext->entityManager->setHorizontalShift(value / 100.0f);
            m_currentContext->openGLRenderer->updateScene();
            ui->depthWidget->setShift(value);
          });

  connect(ui->openGLWidget, &OpenGLWidget::GLInitialized, [this] {
    m_widgetRenderingContext = std::make_unique<RenderingContext>(ui->openGLWidget);
    m_currentContext = m_widgetRenderingContext.get();
    m_currentContext->entityManager->setUserSettings(&m_userSettings);
    m_userSettings.viewerContext.imageWidthPixels =
        m_currentContext->textureManager->getTextureSize().width();

    handleNewImagePair(m_currentContext->textureManager->getImageLeft(), m_currentContext->textureManager->getImageRight(), {});

    connect(
        m_videoSynchronizer.get(),
        &VideoSynchronizer::incomingImagePair,
        [this](const QImage& imgLeft, const QImage& imgRight, std::chrono::microseconds timestamp) {
          handleNewImagePair(imgLeft, imgRight, timestamp);
        });

    connect(ui->videoControls, &VideoControls::playClicked, [this] {
      if (m_videoSynchronizer != nullptr) {
        m_videoSynchronizer->resume();
        ui->videoControls->setDuration(m_videoSynchronizer->videoDuration());
      }
    });
    connect(ui->videoControls, &VideoControls::firstClicked, [this] {
      if (m_videoSynchronizer != nullptr) {
        m_videoSynchronizer->seekTo(std::chrono::microseconds{0});
      }
    });
    connect(ui->videoControls, &VideoControls::pauseClicked, [this] {
      if (m_videoSynchronizer != nullptr) {
        m_videoSynchronizer->pause();
      }
    });
    connect(ui->videoControls, &VideoControls::nextClicked, [this] {
      if (m_videoSynchronizer != nullptr) {
        m_videoSynchronizer->next();
      }
    });
  });

  connect(ui->videoControls,
          &VideoControls::seekingRequested,
          [this](std::chrono::microseconds timestamp) { m_videoSynchronizer->seekTo(timestamp); });
}

MainWindow::~MainWindow() {
  // delete here to make OpenGL context current
  if (m_videoSynchronizer != nullptr) {
    m_videoSynchronizer->stop();
  }
  m_widgetRenderingContext.reset();
  m_windowRenderingContext.reset();
  delete ui;
}

void MainWindow::computeAndUpdate() {
  if (m_imageOperations->inputOutputAdapter.applyAllOperations()) {
    m_currentContext->makeCurrent();
    m_currentContext->entityManager->setFeatures(m_imageOperations->inputOutputAdapter.results.featuresRight,
                                                 m_analyzer->results.disparitiesPercent);
    m_currentContext->doneCurrent();

    ui->depthWidget->setLowValue(m_analyzer->results.minDisparityPercent);
    ui->depthWidget->setHighValue(m_analyzer->results.maxDisparityPercent);

    ui->parametersListView->setParameter("Roll", m_imageOperations->inputOutputAdapter.results.alignment.rollAngleDegrees());
    ui->parametersListView->setParameter("Vertical", m_imageOperations->inputOutputAdapter.results.alignment.verticalOffsetDegrees());
    ui->parametersListView->setParameter("Pan Keystone", m_imageOperations->inputOutputAdapter.results.alignment.panKeystoneDegreesPerMeter());
    ui->parametersListView->setParameter("Tilt Keystone", m_imageOperations->inputOutputAdapter.results.alignment.tiltKeystoneDegreesPerMeter());
    ui->parametersListView->setParameter("Tilt Offset", m_imageOperations->inputOutputAdapter.results.alignment.tiltOffsetPixels());
    ui->parametersListView->setParameter("Zoom", m_imageOperations->inputOutputAdapter.results.alignment.zoomRatioPercent());

    updateConvergenceHint(m_analyzer->results.minDisparityPercent,
                          m_analyzer->results.maxDisparityPercent);
  } else {
    m_currentContext->openGLRenderer->makeCurrent();
    m_currentContext->entityManager->setFeatures({}, {});
    m_currentContext->openGLRenderer->doneCurrent();
  }

  cv::Mat outputImageLeft;
  cv::Mat outputImageRight;
  std::tie(outputImageLeft, outputImageRight) = m_imageOperations->inputOutputAdapter.getOutputImages();

  m_userSettings.viewerContext.imageWidthPixels = outputImageLeft.cols;
  m_currentContext->makeCurrent();
  m_currentContext->textureManager->setImages(Mat2QImage(outputImageLeft), Mat2QImage(outputImageRight));
  m_currentContext->doneCurrent();
  m_currentContext->openGLRenderer->updateScene();
}

void MainWindow::updateConvergenceHint(float minDisparity, float maxDisparity) {
  auto range = maxDisparity - minDisparity;

  // todo: set this in mainwindow, not inside widgets
  auto expectedMin = -1.1f;
  auto expectedMax = 3.1f;
  auto expectedRange = expectedMax - expectedMin;
  auto ratio = range / expectedRange;
  ui->convergenceIndicator->updateHint(ratio);
}

template <class Functor>
void MainWindow::requestImageFilename(Functor f) {
  QString filename =
      QFileDialog::getOpenFileName(this,
                                   tr("Open Image"),
                                   "/home/bedh2102",
                                   tr("Image Files (*.png *.jpg *.bmp *.pbm)",
                                   nullptr, QFileDialog::DontUseNativeDialog));
  if (!filename.isEmpty()) {
    f(filename);
  }
}

template <class Functor>
void MainWindow::requestVideoFilename(Functor f) {
  QString filename = QFileDialog::getOpenFileName(
      this, tr("Open Video"), "/home/bedh2102", "", nullptr, QFileDialog::DontUseNativeDialog);
  if (!filename.isEmpty()) {
    f(filename);
  }
}

void MainWindow::mouseDoubleClickEvent(QMouseEvent* e) {
  if (e->button() == Qt::LeftButton) {
    if (m_openGLWindow == nullptr) {
      m_openGLWindow = std::make_unique<OpenGLWindow>();

      //      QSurfaceFormat format;
      //      format.setRenderableType(QSurfaceFormat::OpenGL);
      //      format.setProfile(QSurfaceFormat::CoreProfile);
      //      format.setVersion(3, 3);
      //      m_openGLWindow->setFormat(format);
      m_openGLWindow->resize(800, 600);

      connect(m_openGLWindow.get(), &OpenGLWindow::GLInitialized, [this] {
        // copy current context (images, features, shift) to window context
        // since everything before the double click will not have been
        // registered
        m_windowRenderingContext = std::make_unique<RenderingContext>(m_openGLWindow.get());
        m_windowRenderingContext->persistState(m_widgetRenderingContext.get(),  //
                                               getCurrentDisplayMode(),         //
                                               m_analyzer->results.stan.featuresRight,
                                               m_analyzer->results.disparitiesPercent,
                                               ui->actionFeatures->isChecked());
        m_windowRenderingContext->entityManager->setUserSettings(&m_userSettings);
        m_currentContext = m_windowRenderingContext.get();
      });

      connect(m_openGLWindow.get(), &OpenGLWindow::onClose, [this] {
        m_widgetRenderingContext->persistState(m_windowRenderingContext.get(),  //
                                               getCurrentDisplayMode(),         //
                                               m_analyzer->results.stan.featuresRight,
                                               m_analyzer->results.disparitiesPercent,
                                               ui->actionFeatures->isChecked());
        m_currentContext = m_widgetRenderingContext.get();
      });
    } else {
      m_windowRenderingContext->persistState(m_widgetRenderingContext.get(),  //
                                             getCurrentDisplayMode(),         //
                                             m_analyzer->results.stan.featuresRight,
                                             m_analyzer->results.disparitiesPercent,
                                             ui->actionFeatures->isChecked());
      m_currentContext = m_windowRenderingContext.get();
    }

    m_openGLWindow->showFullScreen();
  }
}

EntityManager::DisplayMode MainWindow::getCurrentDisplayMode() {
  if (ui->actionAnaglyph->isChecked()) {
    return EntityManager::DisplayMode::Anaglyph;
  }
  if (ui->actionOpacity->isChecked()) {
    return EntityManager::DisplayMode::Opacity;
  }
  if (ui->actionInterlaced->isChecked()) {
    return EntityManager::DisplayMode::Interlaced;
  }
  if (ui->actionSide_by_side->isChecked()) {
    return EntityManager::DisplayMode::SideBySide;
  }
  if (ui->actionLeft->isChecked()) {
    return EntityManager::DisplayMode::Left;
  }
  if (ui->actionRight->isChecked()) {
    return EntityManager::DisplayMode::Right;
  }

  return EntityManager::DisplayMode::Anaglyph;
}

void MainWindow::closeEvent(QCloseEvent* /*event*/) {
  if (m_disparitySettingsDialog != nullptr) {
    m_disparitySettingsDialog->close();
  }
}

void MainWindow::updateInputMode() {
  // hide all menus
  ui->actionOpenLeftImage->setVisible(false);
  ui->actionOpenRightImage->setVisible(false);
  ui->actionOpenImage->setVisible(false);
  ui->actionOpenLeftVideo->setVisible(false);
  ui->actionOpenRightVideo->setVisible(false);
  ui->actionOpenVideo->setVisible(false);

  // show only relevant menus
  if (!ui->actionFormatSeparateFiles->isChecked()) {
    if (ui->actionInputImage->isChecked()) {
      ui->actionOpenImage->setVisible(true);
    } else if (ui->actionInputVideo->isChecked()) {
      ui->actionOpenVideo->setVisible(true);
    }
  } else {
    if (ui->actionInputImage->isChecked()) {
      ui->actionOpenLeftImage->setVisible(true);
      ui->actionOpenRightImage->setVisible(true);
    } else if (ui->actionInputVideo->isChecked()) {
      ui->actionOpenLeftVideo->setVisible(true);
      ui->actionOpenRightVideo->setVisible(true);
    } else if (ui->actionInputLive->isChecked()) {
      // load live camera
      ui->videoControls->setVisible(false);
      m_videoSynchronizer->loadLiveCamera();
    }
  }

  m_videoSynchronizer->stop();

  if (ui->actionInputLive->isChecked()) {
    // load live camera
    ui->videoControls->setVisible(false);
    m_videoSynchronizer->loadLiveCamera();
  }

  // no value smoothing for image
  if (ui->actionInputImage->isChecked()) {
    m_analyzer->setMinimumNumberOfInliers(0);
    ui->videoControls->setVisible(false);
  } else {
    m_analyzer->setMinimumNumberOfInliers(m_analyzerMinNbInliers);
    ui->videoControls->setVisible(true);
  }
}

void MainWindow::updateStereo3DFormat() {
  bool halfResolution = ui->actionFormatHalfResolution->isChecked();

  s3d::Stereo3DFormat format{};
  if (ui->actionFormatSeparateFiles->isChecked()) {
    format = s3d::Stereo3DFormat::Separate;
  } else if (ui->actionFormatSideBySide->isChecked()) {
    format = halfResolution ? s3d::Stereo3DFormat::SideBySideHalf : s3d::Stereo3DFormat::SideBySide;
  } else if (ui->actionFormatAboveBelow->isChecked()) {
    format = halfResolution ? s3d::Stereo3DFormat::AboveBelowHalf : s3d::Stereo3DFormat::AboveBelow;
  }
  if (m_videoSynchronizer != nullptr) {
    m_videoSynchronizer->setStereoVideoFormat(format);
    m_stereoFormat = format;
  }

  // update image to demux
  if (ui->actionInputImage->isChecked()) {
    if (ui->actionFormatSeparateFiles->isChecked() && m_imageLeftReady && m_imageRightReady) {
      handleNewImagePair(m_imageLeft, m_imageRight, {});
    } else if (!ui->actionFormatSeparateFiles->isChecked()) {
      demuxImage(m_imageLeft);
    }
  }
}

void MainWindow::handleNewImagePair(const QImage& imgLeft,
                                    const QImage& imgRight,
                                    std::chrono::microseconds timestamp) {
  m_imageLeftReady = false;
  m_imageRightReady = false;

  // don't display video frames when in image mode
  if (ui->actionInputImage->isChecked() && timestamp != std::chrono::microseconds{}) {
    return;
  }

  m_imageOperations->inputOutputAdapter.setInputImages(QImage2Mat(imgLeft), QImage2Mat(imgRight));
  ui->videoControls->updateSlider(timestamp);
  m_currentContext->entityManager->setUserSettings(&m_userSettings);
  computeAndUpdate();
}

bool MainWindow::stereoFormatChanged() {
  return m_stereoDemuxer != nullptr && m_stereoDemuxer->getStereoFormat() != m_stereoFormat;
}

void MainWindow::updateStereoDemuxer() {
  m_stereoDemuxer = m_stereoDemuxerFactory->createStereoDemuxerQImage(m_stereoFormat);
}

bool MainWindow::stereoDemuxerRequired() {
  return m_stereoFormat != s3d::Stereo3DFormat::Separate;
}

void MainWindow::demuxImage(const QImage& image) {
  m_imageLeftReady = false;
  m_imageRightReady = false;

  if ((m_stereoDemuxer == nullptr && stereoDemuxerRequired()) || stereoFormatChanged()) {
    updateStereoDemuxer();
  }

  QImage imgLeft, imgRight;
  std::tie(imgLeft, imgRight) = m_stereoDemuxer->demuxQImage(image);

  handleNewImagePair(imgLeft, imgRight, {});
}
