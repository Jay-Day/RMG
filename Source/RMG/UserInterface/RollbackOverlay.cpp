#include "RollbackOverlay.h"
#include "../../RMG-Core/m64p/CoreApi.hpp"
#include "../../RMG-Core/Netplay.hpp"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFont>
#include <QPalette>
#include <QApplication>

RollbackOverlay::RollbackOverlay(QWidget* parent)
    : QWidget(parent)
{
    setupUI();
    styleOverlay();
    
    // Set up update timer (update every 250ms)
    m_updateTimer = new QTimer(this);
    connect(m_updateTimer, &QTimer::timeout, this, &RollbackOverlay::updateMetrics);
    m_updateTimer->start(250);
    
    // Initial update
    updateMetrics();
}

RollbackOverlay::~RollbackOverlay()
{
    if (m_updateTimer) {
        m_updateTimer->stop();
        delete m_updateTimer;
    }
}

void RollbackOverlay::setVisible(bool visible)
{
    QWidget::setVisible(visible);
    
    // Start or stop the timer as needed
    if (visible) {
        m_updateTimer->start(250);
    } else {
        m_updateTimer->stop();
    }
}

void RollbackOverlay::updateMetrics()
{
    // Get current metrics from the core
    int rollbackFrames = 0;
    int totalRollbacks = 0;
    int predictedFrames = 0;
    int maxRollbackFrames = 0;
    float avgRollbackFrames = 0.0f;
    int pingMs = 0;
    int remoteFrameAdvantage = 0;
    
    if (CoreRollbackNetplayGetMetrics(&rollbackFrames, &totalRollbacks, &predictedFrames,
                                      &maxRollbackFrames, &avgRollbackFrames,
                                      &pingMs, &remoteFrameAdvantage)) {
        // Update labels with current values
        m_pingLabel->setText(QString("Ping: %1 ms").arg(pingMs));
        m_rollbackCountLabel->setText(QString("Total Rollbacks: %1").arg(totalRollbacks));
        m_predictionLabel->setText(QString("Predicted Frames: %1").arg(predictedFrames));
        m_maxRollbackLabel->setText(QString("Max Rollback: %1 frames").arg(maxRollbackFrames));
        m_avgRollbackLabel->setText(QString("Avg Rollback: %1 frames").arg(avgRollbackFrames, 0, 'f', 1));
        
        // Color-code frame advantage
        QString frameAdvText = QString("Frame Advantage: %1").arg(remoteFrameAdvantage);
        m_frameAdvantageLabel->setText(frameAdvText);
        
        // Set colors based on values
        QPalette palette = m_pingLabel->palette();
        
        // Ping color (green < 50ms, yellow < 100ms, red >= 100ms)
        if (pingMs < 50) {
            palette.setColor(QPalette::WindowText, Qt::green);
        } else if (pingMs < 100) {
            palette.setColor(QPalette::WindowText, Qt::yellow);
        } else {
            palette.setColor(QPalette::WindowText, Qt::red);
        }
        m_pingLabel->setPalette(palette);
        
        // Rollback color (green < 2, yellow < 5, red >= 5)
        palette = m_maxRollbackLabel->palette();
        if (maxRollbackFrames < 2) {
            palette.setColor(QPalette::WindowText, Qt::green);
        } else if (maxRollbackFrames < 5) {
            palette.setColor(QPalette::WindowText, Qt::yellow);
        } else {
            palette.setColor(QPalette::WindowText, Qt::red);
        }
        m_maxRollbackLabel->setPalette(palette);
    }
}

void RollbackOverlay::setupUI()
{
    // Create main layout
    QVBoxLayout* mainLayout = new QVBoxLayout(this);
    mainLayout->setSpacing(5);
    mainLayout->setContentsMargins(10, 10, 10, 10);
    
    // Title and close button in horizontal layout
    QHBoxLayout* titleLayout = new QHBoxLayout();
    m_titleLabel = new QLabel("Rollback Netcode Metrics", this);
    QFont titleFont = m_titleLabel->font();
    titleFont.setBold(true);
    titleFont.setPointSize(12);
    m_titleLabel->setFont(titleFont);
    
    m_closeButton = new QPushButton("×", this);
    m_closeButton->setFixedSize(24, 24);
    m_closeButton->setFlat(true);
    connect(m_closeButton, &QPushButton::clicked, this, [this]() { setVisible(false); });
    
    titleLayout->addWidget(m_titleLabel);
    titleLayout->addStretch();
    titleLayout->addWidget(m_closeButton);
    mainLayout->addLayout(titleLayout);
    
    // Add separator line
    QFrame* line = new QFrame(this);
    line->setFrameShape(QFrame::HLine);
    line->setFrameShadow(QFrame::Sunken);
    mainLayout->addWidget(line);
    
    // Create metric labels
    m_pingLabel = new QLabel("Ping: 0 ms", this);
    m_rollbackCountLabel = new QLabel("Total Rollbacks: 0", this);
    m_predictionLabel = new QLabel("Predicted Frames: 0", this);
    m_maxRollbackLabel = new QLabel("Max Rollback: 0 frames", this);
    m_avgRollbackLabel = new QLabel("Avg Rollback: 0.0 frames", this);
    m_frameAdvantageLabel = new QLabel("Frame Advantage: 0", this);
    
    // Add labels to layout
    mainLayout->addWidget(m_pingLabel);
    mainLayout->addWidget(m_rollbackCountLabel);
    mainLayout->addWidget(m_predictionLabel);
    mainLayout->addWidget(m_maxRollbackLabel);
    mainLayout->addWidget(m_avgRollbackLabel);
    mainLayout->addWidget(m_frameAdvantageLabel);
    
    // Set fixed size
    setFixedSize(300, 200);
}

void RollbackOverlay::styleOverlay()
{
    // Set background color and border
    setAutoFillBackground(true);
    
    QPalette pal = palette();
    pal.setColor(QPalette::Window, QColor(32, 32, 40, 200)); // Semi-transparent dark background
    
    // Set text color
    pal.setColor(QPalette::WindowText, Qt::white);
    
    setPalette(pal);
    
    // Set rounded corners with stylesheet
    setStyleSheet("QWidget { border: 1px solid #6060A0; border-radius: 5px; }");
    
    // Align to top right of parent
    if (parentWidget()) {
        QPoint topRight = parentWidget()->rect().topRight();
        move(topRight.x() - width() - 10, topRight.y() + 10);
    }
} 