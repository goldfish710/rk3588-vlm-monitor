#include "mainwindow.h"
#include "ui_mainwindow.h"

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
    , ui(new Ui::MainWindow)
{
    ui->setupUi(this);

    //选择页面按钮
    connect(ui->btnSelectMain, &QPushButton::clicked, this, &MainWindow::onSelectMain);
    connect(ui->btnSelectSub,  &QPushButton::clicked, this, &MainWindow::onSelectSub);
    connect(ui->btnSettings,   &QPushButton::clicked, this, &MainWindow::onSettings);

    //视频页面按钮
    connect(ui->btnBack,       &QPushButton::clicked, this, &MainWindow::onBack);
    connect(ui->btnConnect,    &QPushButton::clicked, this, &MainWindow::onConnect);
    connect(ui->btnDisconnect, &QPushButton::clicked, this, &MainWindow::onDisconnect);
    connect(ui->btnScreenshot, &QPushButton::clicked, this, &MainWindow::onScreenshot);
    connect(ui->btnRecord,     &QPushButton::clicked, this, &MainWindow::onRecord);

    h265decoder = new VideoDecoder(MAIN_STREAM_URL, this);
    h264decoder = new VideoDecoder(SUB_STREAM_URL, this);

    connect(h265decoder, &VideoDecoder::frameReady, this, &MainWindow::onframeReady);
    connect(h264decoder, &VideoDecoder::frameReady, this, &MainWindow::onframeReady);
    connect(h265decoder, &VideoDecoder::recordFinished, this, &MainWindow::onRecordFinished);
    connect(h264decoder, &VideoDecoder::recordFinished, this, &MainWindow::onRecordFinished);

    //从 QSettings 加载截图目录，默认为程序目录下的 screenshots/
    QSettings settings("LLClient", "LLClient");
    screenshotDir = settings.value("screenshotDir",
        QCoreApplication::applicationDirPath() + "/screenshots").toString();
    QDir().mkpath(screenshotDir);
}

MainWindow::~MainWindow()
{
    delete ui;
    if(h265decoder)
    {
        delete h265decoder;
    }
    if(h264decoder)
    {
        delete h264decoder;
    }
}

//选择主码流
void MainWindow::onSelectMain()
{
    currentDecoder = h265decoder;
    ui->videoLabel->setText("主码流 H.265 1920×1080");
    ui->stackedWidget->setCurrentIndex(1);
    statusBar()->showMessage("已选择主码流");
}

//选择子码流
void MainWindow::onSelectSub()
{
    currentDecoder = h264decoder;
    ui->videoLabel->setText("子码流 H.264 640×360");
    ui->stackedWidget->setCurrentIndex(1);
    statusBar()->showMessage("已选择子码流");
}

//连接
void MainWindow::onConnect()
{
    if(!currentDecoder) return;
    ui->btnConnect->setEnabled(false);
    ui->btnDisconnect->setEnabled(true);
    ui->btnScreenshot->setEnabled(true);
    ui->btnRecord->setEnabled(true);
    currentDecoder->start();
    statusBar()->showMessage("已连接");
}

//断开
void MainWindow::onDisconnect()
{
    if(!currentDecoder) return;
    if(isRecording)
    {
        currentDecoder->stopRecord();
        isRecording = false;
        ui->btnRecord->setText("录像");
    }
    currentDecoder->stop();
    currentDecoder->wait();
    ui->btnConnect->setEnabled(true);
    ui->btnDisconnect->setEnabled(false);
    ui->btnScreenshot->setEnabled(false);
    ui->btnRecord->setEnabled(false);
    statusBar()->showMessage("已断开");
}

//截图
void MainWindow::onScreenshot()
{
    if(currentFrame.isNull()) return;

    QString filename = QDateTime::currentDateTime().toString("yyyyMMdd_hhmmss") + ".png";
    QString path = screenshotDir + "/" + filename;

    QImage img = currentFrame;
    QtConcurrent::run([img, path]() {
        img.save(path);
    });

    statusBar()->showMessage("截图已保存: " + path);
}

//设置截图目录
void MainWindow::onSettings()
{
    QString dir = QFileDialog::getExistingDirectory(this, "选择截图保存目录", screenshotDir);
    if(!dir.isEmpty())
    {
        screenshotDir = dir;
        QSettings settings("LLClient", "LLClient");
        settings.setValue("screenshotDir", screenshotDir);
        statusBar()->showMessage("截图目录已设置: " + screenshotDir);
    }
}

//录像
void MainWindow::onRecord()
{
    if(!currentDecoder) return;

    if(!isRecording)
    {
        QString filename = QDateTime::currentDateTime().toString("yyyyMMdd_hhmmss") + ".mp4";
        QString path = screenshotDir + "/" + filename;
        currentDecoder->startRecord(path);
        isRecording = true;
        ui->btnRecord->setText("停止录像");
        statusBar()->showMessage("录像中...");
    }
    else
    {
        currentDecoder->stopRecord();
        isRecording = false;
        ui->btnRecord->setText("录像");
    }
}

//录像完成回调
void MainWindow::onRecordFinished(const QString &filePath)
{
    statusBar()->showMessage("录像已保存: " + filePath);
}

//返回选择页面
void MainWindow::onBack()
{
    if(currentDecoder)
    {
        if(isRecording)
        {
            currentDecoder->stopRecord();
            isRecording = false;
            ui->btnRecord->setText("录像");
        }
        currentDecoder->stop();
        currentDecoder->wait();
    }
    currentDecoder = nullptr;

    //重置按钮状态
    ui->btnConnect->setEnabled(true);
    ui->btnDisconnect->setEnabled(false);
    ui->btnScreenshot->setEnabled(false);
    ui->btnRecord->setEnabled(false);
    ui->videoLabel->clear();
    ui->videoLabel->setText("未连接");

    ui->stackedWidget->setCurrentIndex(0);
    statusBar()->showMessage("请选择码流");
}

//显示画面
void MainWindow::onframeReady(const QImage &image)
{
    currentFrame = image;
    ui->videoLabel->setPixmap(QPixmap::fromImage(image).scaled(ui->videoLabel->size(),Qt::KeepAspectRatio,Qt::SmoothTransformation));
}
