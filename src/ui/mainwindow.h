#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include <QSystemTrayIcon>
#include <QSignalMapper>
#include <QSettings>
#include <QMap>
#include <QModelIndex>
#include <QStringList>
#include <QMouseEvent>
#include <QEvent>
#include <QPoint>
#include <QTimer>
#include <QElapsedTimer>

#include "mpvhandler.h"
#include "updatemanager.h"
#include "widgets/dimdialog.h"

namespace Ui {
class MainWindow;
}

class MainWindow : public QMainWindow
{
    Q_OBJECT
public:
    explicit MainWindow(QWidget *parent = 0);
    ~MainWindow();

    void Load(QString f = QString());

protected:
    void LoadSettings();
    void SaveSettings();
    void LoadKeybindings();                         // setup the keybindings from settings
    void WriteDefaultKeybindings();                 // setup the baka-mplayer default keybindings

    void dragEnterEvent(QDragEnterEvent *event);    // drag file into
    void dropEvent(QDropEvent *event);              // drop file into
    void mousePressEvent(QMouseEvent *event);       // pressed mouse down
    void mouseReleaseEvent(QMouseEvent *event);     // released mouse up
    void mouseMoveEvent(QMouseEvent *event);        // moved mouse on the form
    void mouseDoubleClickEvent(QMouseEvent *event); // double clicked the form
    bool eventFilter(QObject *obj, QEvent *event);  // event filter (get mouse move events from mpvFrame)

    void SetPlaybackControls(bool enable);          // macro to enable/disable playback controls

private slots:

    void FullScreen(bool fs);                       // makes window fullscreen
    void ShowPlaylist(bool visible);                // sets the playlist visibility
    void HideAlbumArt(bool hide);                   // hides the album art
    void FitWindow(int percent, bool msg = false);  // fit the window the the specified percent
    void SetAspectRatio(QString aspect);            // set the aspect ratio to specified proportions
    void DimLights(bool dim);                       // grays out the rest of the screen with LightDialog
    void AlwaysOnTop(bool ontop);                   // set always on top window state
    void TakeScreenshot(bool subs);                 // take a screenshot
    void ShowScreenshotMessage(bool subs);          // show the screenshot status message
    void UpdateRecentFiles();                       // populate recentFiles menu

    void OpenRecent(int index);                     // loads the file from recent files
    void BakaOutput(QString output);                // handles message output for [baka]
    void ExecuteCommand(QString command);           // drives the command engine
    void HandleBakaCommand(QStringList cmdList);    // handles commands that begin with baka
    void HandleInvalidCommand(QString command);
    void HandleInvalidParameter(QString parameter);

    // command functionality
    void HandlePlayPause();
    void HandlePlayNext();
    void HandlePlayPrevious();
    void HandleRewind();
    void HandleStop();
    void HandleMove();
    void HandleBoss();
    void HandleOpenFileDialog();
    void HandleOpenURLDialog();
    void HandleOpenClipboard();
    void HandleJumpDialog();
    void HandleShowInFolder();
    void HandleNewPlayer();
    void HandleMediaInfoDialog();
    void HandlePreferencesDialog();
    void HandleUpdateDialog();
    void HandleOnlineHelp();
    void HandleAboutDialog();
    void HandleAboutQtDialog();
    void HandleExit();

private:
    Ui::MainWindow  *ui;
    QSettings       *settings;
    MpvHandler      *mpv;
    UpdateManager   *update;

    QPoint          origPos,
                    lastMousePos;
    bool            pathChanged,
                    menuVisible,
                    firstItem,
                    init;
    QTimer          *autohide;
    QElapsedTimer   *moveTimer;

    QSystemTrayIcon *sysTrayIcon;
    QMenu           *trayIconMenu;
    DimDialog       *dimDialog;

    // variables
    QMap<QString, QString> input;
    QStringList recent;
    QString onTop;
    int autoFit,
        maxRecent;
    bool hidePopup,
         remaining,
         screenshotDialog,
         debug;

public slots:
    void setOnTop(QString s)         { emit onTopChanged(onTop = s); }
    void setAutoFit(int b)           { emit autoFitChanged(autoFit = b); }
    void setHidePopup(bool b)        { emit hidePopupChanged(hidePopup = b); }
    void setRemaining(bool b)        { emit remainingChanged(remaining = b); }
    void setScreenshotDialog(bool b) { emit screenshotDialogChanged(screenshotDialog = b); }
    void setDebug(bool b)            { emit debugChanged(debug = b); }

signals:
    void onTopChanged(QString);
    void autoFitChanged(int);
    void hidePopupChanged(bool);
    void remainingChanged(bool);
    void screenshotDialogChanged(bool);
    void debugChanged(bool);
};

#endif // MAINWINDOW_H
