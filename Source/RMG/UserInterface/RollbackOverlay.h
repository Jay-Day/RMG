#ifndef ROLLBACKOVERLAY_H
#define ROLLBACKOVERLAY_H

#include <QWidget>
#include <QLabel>
#include <QTimer>
#include <QPushButton>

// Rollback overlay widget that displays real-time rollback metrics
class RollbackOverlay : public QWidget
{
    Q_OBJECT

public:
    explicit RollbackOverlay(QWidget* parent = nullptr);
    ~RollbackOverlay();

    // Set visibility of the overlay
    void setVisible(bool visible);
    
    // Update metrics display
    void updateMetrics();

private:
    // UI elements
    QLabel* m_titleLabel;
    QLabel* m_pingLabel;
    QLabel* m_rollbackCountLabel;
    QLabel* m_predictionLabel;
    QLabel* m_maxRollbackLabel;
    QLabel* m_avgRollbackLabel;
    QLabel* m_frameAdvantageLabel;
    
    QPushButton* m_closeButton;
    
    // Update timer
    QTimer* m_updateTimer;
    
    // Initialize UI elements
    void setupUI();
    
    // Style the overlay
    void styleOverlay();
};

#endif // ROLLBACKOVERLAY_H 