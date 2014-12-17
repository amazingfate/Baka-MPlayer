
#include "mainwindow.h"
#include "ui_mainwindow.h"

#include <QCoreApplication>
#include <QMessageBox>
#include <QMimeData>
#include <QTime>
#include <QFileDialog>
#include <QClipboard>
#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QDesktopWidget>
#include <QAction>
#include <QShortcut>
#include <QProcess>
#include <QIcon>
#include <QWindow>
#include <QCheckBox>

#if defined(Q_OS_UNIX) || defined(Q_OS_LINUX)
#include <QX11Info>
#include <X11/Xlib.h>
#else
#include <windows.h>
#endif

#include "aboutdialog.h"
#include "infodialog.h"
#include "locationdialog.h"
#include "jumpdialog.h"
#include "inputdialog.h"
#include "updatedialog.h"
#include "preferencesdialog.h"
#include "screenshotdialog.h"
#include "util.h"

using namespace BakaUtil;

MainWindow::MainWindow(QWidget *parent):
    QMainWindow(parent),
    ui(new Ui::MainWindow),
    firstItem(false),
    init(false),
    autohide(new QTimer(this)),
    moveTimer(nullptr)
{
    QAction *action;

#if defined(Q_OS_LINUX) || defined(Q_OS_UNIX) // if on x11, dim lights requires a compositing manager, make dimDialog NULL if there is none
    QString tmp = "_NET_WM_CM_S"+QString::number(QX11Info::appScreen());
    Atom a = XInternAtom(QX11Info::display(), tmp.toUtf8().constData(), false);
    if(a && XGetSelectionOwner(QX11Info::display(), a)) // hack for QX11Info::isCompositingManagerRunning()
        dimDialog = new DimDialog(); // dimdialog must be initialized before ui is setup
    else
        dimDialog = nullptr;
#else
    dimDialog = new DimDialog(); // dimDialog must be initialized before ui is setup
#endif
    ui->setupUi(this);
    ShowPlaylist(false);
    addActions(ui->menubar->actions()); // makes menubar shortcuts work even when menubar is hidden

    // initialize managers/handlers
#if defined(Q_OS_WIN) // saves to $(application directory)\${SETTINGS_FILE}.ini
    settings = new QSettings(QApplication::applicationDirPath()+"\\"+SETTINGS_FILE+".ini", QSettings::IniFormat,this);
#else // saves to  ~/.config/${SETTINGS_FILE}.ini on linux
    settings = new QSettings(QSettings::IniFormat, QSettings::UserScope, SETTINGS_FILE, QString(), this);
#endif
    mpv = new MpvHandler(ui->mpvFrame->winId(), this);
    update = new UpdateManager(this);

    // initialize other ui elements
    // note: trayIcon does not work in my environment--known qt bug
    // see: https://bugreports.qt-project.org/browse/QTBUG-34364
    // todo: tray menu/tooltip
    sysTrayIcon = new QSystemTrayIcon(qApp->windowIcon(), this);
    ui->mpvFrame->installEventFilter(this); // capture events on mpvFrame in the eventFilter function

    // setup signals & slots

    // mainwindow
    connect(this, &MainWindow::onTopChanged,
            [=](QString onTop)
            {
                if(onTop == "never")
                    AlwaysOnTop(false);
                else if(onTop == "always")
                    AlwaysOnTop(true);
                else if(onTop == "playing" && mpv->getPlayState() > 0)
                    AlwaysOnTop(true);
            });

    connect(sysTrayIcon, &QSystemTrayIcon::activated,
            [=](QSystemTrayIcon::ActivationReason reason)
            {
                if(reason == QSystemTrayIcon::Trigger)
                {
                    if(!hidePopup)
                    {
                        if(mpv->getPlayState() == Mpv::Playing)
                            sysTrayIcon->showMessage("Baka MPlayer", tr("Playing"), QSystemTrayIcon::NoIcon, 4000);
                        else if(mpv->getPlayState() == Mpv::Paused)
                            sysTrayIcon->showMessage("Baka MPlayer", tr("Paused"), QSystemTrayIcon::NoIcon, 4000);
                    }
                    mpv->PlayPause(ui->playlistWidget->CurrentItem());
                }

            });

//    connect(this, &MainWindow::hidePopupChanged,
//            [=](bool b)
//    {
//    });

    connect(this, &MainWindow::debugChanged,
            [=](bool b)
            {
                mpv->Debug(b);
                ui->actionShow_D_ebug_Output->setChecked(b);
                ui->verticalWidget->setVisible(b);
            });

    connect(autohide, &QTimer::timeout, // cursor autohide
            [=]
            {
                setCursor(QCursor(Qt::BlankCursor));
                autohide->stop();
            });

    // mpv

    connect(mpv, &MpvHandler::playlistChanged,
            [=](const QStringList &list)
            {
                ui->playlistWidget->Populate(list);
                firstItem = true;

                if(list.length() > 1)
                {
                    ui->actionSh_uffle->setEnabled(true);
                    ui->actionStop_after_Current->setEnabled(true);
                }
                else
                {
                    ui->actionSh_uffle->setEnabled(false);
                    ui->actionStop_after_Current->setEnabled(false);
                }

                if(list.length() > 0)
                    ui->menuR_epeat->setEnabled(true);
                else
                    ui->menuR_epeat->setEnabled(false);
            });

    connect(mpv, &MpvHandler::fileInfoChanged,
            [=](const Mpv::FileInfo &fileInfo)
            {
                if(mpv->getPlayState() > 0)
                {
                    if(fileInfo.media_title == "")
                        setWindowTitle("Baka MPlayer");
                    else if(fileInfo.media_title == "-")
                        setWindowTitle("Baka MPlayer: stdin"); // todo: disable playlist?
                    else
                        setWindowTitle(fileInfo.media_title);

                    // todo: deal with streamed input for which we do not know the length
                    ui->seekBar->setTracking(fileInfo.length);

                    if(!remaining)
                        ui->remainingLabel->setText(FormatTime(fileInfo.length, fileInfo.length));
                }
            });

    connect(mpv, &MpvHandler::trackListChanged,
            [=](const QList<Mpv::Track> &trackList)
    {
        if(mpv->getPlayState() > 0)
        {
            QAction *action;
            bool video = false,
                 albumArt = false;

            ui->menuSubtitle_Track->clear();
            ui->menuSubtitle_Track->addAction(ui->action_Add_Subtitle_File);
            ui->menuAudio_Tracks->clear();
            for(auto &track : trackList)
            {
                if(track.type == "sub")
                {
                    action = ui->menuSubtitle_Track->addAction(tr("%0: %1 (%2)").arg(QString::number(track.id), track.title, track.lang).replace("&", "&&"));
                    connect(action, &QAction::triggered,
                            [=]
                            {
                                // basically, if you uncheck the selected subtitle id, we hide subtitles
                                // when you check a subtitle id, we make sure subtitles are showing and set it
                                if(mpv->getSid() == track.id)
                                {
                                    if(mpv->getSubtitleVisibility())
                                    {
                                        mpv->ShowSubtitles(false);
                                        return;
                                    }
                                    else
                                        mpv->ShowSubtitles(true);
                                }
                                else if(!mpv->getSubtitleVisibility())
                                    mpv->ShowSubtitles(true);
                                mpv->Sid(track.id);
                                mpv->ShowText(tr("Sub %0: %1 (%2)").arg(QString::number(track.id), track.title, track.lang));
                            });
                }
                else if(track.type == "audio")
                {
                    action = ui->menuAudio_Tracks->addAction(tr("%0: %1 (%2)").arg(QString::number(track.id), track.title, track.lang).replace("&", "&&"));
                    connect(action, &QAction::triggered,
                            [=]
                            {
                                if(mpv->getAid() != track.id) // don't allow selection of the same track
                                {
                                    mpv->Aid(track.id);
                                    mpv->ShowText(tr("Audio %0: %1 (%2)").arg(QString::number(track.id), track.title, track.lang));
                                }
                                else
                                    action->setChecked(true); // recheck the track
                            });
                }
                else if(track.type == "video") // video track
                {
                    if(!track.albumart) // isn't album art
                        video = true;
                    else
                        albumArt = true;
                }
            }
            if(video)
            {
                // if we were hiding album art, show it--we've gone to a video
                if(ui->mpvFrame->styleSheet() != QString()) // remove filler album art
                    ui->mpvFrame->setStyleSheet("");
                if(ui->action_Hide_Album_Art->isChecked())
                    HideAlbumArt(false);
                ui->action_Hide_Album_Art->setEnabled(false);
                ui->menuSubtitle_Track->setEnabled(true);
                if(ui->menuSubtitle_Track->actions().count() > 1)
                {
                    ui->menuFont_Si_ze->setEnabled(true);
                    ui->actionShow_Subtitles->setEnabled(true);
                    ui->actionShow_Subtitles->setChecked(mpv->getSubtitleVisibility());
                }
                else
                {
                    ui->menuFont_Si_ze->setEnabled(false);
                    ui->actionShow_Subtitles->setEnabled(false);
                }
                ui->menuAudio_Tracks->setEnabled((ui->menuAudio_Tracks->actions().count() > 0));
                if(ui->menuAudio_Tracks->actions().count() == 1)
                    ui->menuAudio_Tracks->actions().first()->setEnabled(false);
                ui->menuTake_Screenshot->setEnabled(true);
                ui->menuFit_Window->setEnabled(true);
                ui->menuAspect_Ratio->setEnabled(true);
                ui->action_Frame_Step->setEnabled(true);
                ui->actionFrame_Back_Step->setEnabled(true);
            }
            else
            {
                if(!albumArt)
                {
                    // put in filler albumArt
                    if(ui->mpvFrame->styleSheet() == QString())
                        ui->mpvFrame->setStyleSheet("background-image:url(:/img/album_art.png);background-repeat:no-repeat;background-position:center;");
                }
                ui->action_Hide_Album_Art->setEnabled(true);
                ui->menuAudio_Tracks->setEnabled((ui->menuAudio_Tracks->actions().count() > 1));
                ui->menuSubtitle_Track->setEnabled(false);
                ui->menuFont_Si_ze->setEnabled(false);
                ui->actionShow_Subtitles->setEnabled(false);
                ui->menuTake_Screenshot->setEnabled(false);
                ui->menuFit_Window->setEnabled(false);
                ui->menuAspect_Ratio->setEnabled(false);
                ui->action_Frame_Step->setEnabled(false);
                ui->actionFrame_Back_Step->setEnabled(false);


                if(sysTrayIcon->isVisible() && !hidePopup)
                {
                    // todo: use {artist} - {title}
                    sysTrayIcon->showMessage("Baka MPlayer", mpv->getFileInfo().media_title, QSystemTrayIcon::NoIcon, 4000);
                }
            }
        }
    });

    connect(mpv, &MpvHandler::chaptersChanged,
            [=](const QList<Mpv::Chapter> &chapters)
    {
        if(mpv->getPlayState() > 0)
        {
            QAction *action;
            QList<int> ticks;
            int n = 1,
                N = chapters.length();
            ui->menu_Chapters->clear();
            for(auto &ch : chapters)
            {
                action = ui->menu_Chapters->addAction(tr("%0: %1").arg(FormatNumberWithAmpersand(n, N), ch.title),
                                                      NULL,
                                                      NULL,
                                                      (n <= 9 ? QKeySequence("Ctrl+"+QString::number(n)) : QKeySequence())
                                                      );
                connect(action, &QAction::triggered,
                        [=]
                        {
                            mpv->Seek(ch.time);
                        });
                ticks.push_back(ch.time);
                n++;
            }
            if(ui->menu_Chapters->actions().count() == 0)
            {
                ui->menu_Chapters->setEnabled(false);
                ui->action_Next_Chapter->setEnabled(false);
                ui->action_Previous_Chapter->setEnabled(false);
            }
            else
            {
                ui->menu_Chapters->setEnabled(true);
                ui->action_Next_Chapter->setEnabled(true);
                ui->action_Previous_Chapter->setEnabled(true);
            }

            ui->seekBar->setTicks(ticks);
        }
    });

    connect(mpv, &MpvHandler::playStateChanged,
            [=](Mpv::PlayState playState)
            {
                switch(playState)
                {
                case Mpv::Loaded: // todo: show the user we are loading their file?
                    break;

                case Mpv::Started:
                    if(!init) // will only happen the first time a file is loaded.
                    {
                        ui->action_Play->setEnabled(true);
                        ui->playButton->setEnabled(true);
                        ui->playlistButton->setEnabled(true);
                        ui->action_Show_Playlist->setEnabled(true);
                        init = true;
                    }
                    if(pathChanged && autoFit)
                    {
                        FitWindow(autoFit);
                        pathChanged = false;
                    }
                    SetPlaybackControls(true);
                    mpv->Play();
                case Mpv::Playing:
                    ui->playButton->setIcon(QIcon(":/img/default_pause.svg"));
                    ui->action_Play->setText(tr("&Pause"));
                    if(onTop == "playing")
                        AlwaysOnTop(true);
                    break;

                case Mpv::Paused:
                case Mpv::Stopped:
                    ui->playButton->setIcon(QIcon(":/img/default_play.svg"));
                    ui->action_Play->setText(tr("&Play"));
                    if(ui->actionWhen_Playing->isChecked())
                        AlwaysOnTop(false);
                    break;

                case Mpv::Idle:
                    if(init)
                    {
                        if(ui->action_This_File->isChecked())
                            mpv->PlayFile(mpv->getFile()); // restart file
                        else if(ui->playlistWidget->currentRow() >= ui->playlistWidget->count()-1 ||
                           ui->actionStop_after_Current->isChecked())
                        {
                            if(ui->action_Playlist->isChecked() && ui->playlistWidget->count() > 0)
                                mpv->PlayFile(ui->playlistWidget->FirstItem()); // restart playlist
                            else
                            {
                                setWindowTitle("Baka MPlayer");
                                SetPlaybackControls(false);
                                ui->seekBar->setTracking(0);
                                ui->actionStop_after_Current->setChecked(false);
                                if(ui->mpvFrame->styleSheet() != QString()) // remove filler album art
                                    ui->mpvFrame->setStyleSheet("");
                            }
                        }
                        else
                            mpv->PlayFile(ui->playlistWidget->NextItem());
                    }
                    break;
                }
            });

    connect(mpv, &MpvHandler::pathChanged,
            [=]()
            {
                pathChanged = true;
            });

    connect(mpv, &MpvHandler::fileChanged,
            [=](QString f)
            {
                if(!firstItem)
                    ui->playlistWidget->SelectItem(f);
                else
                {
                    ui->playlistWidget->SelectItem(f, true);
                    ui->playlistWidget->ShowAll(!ui->hideFilesButton->isChecked());
                    firstItem = false;
                }

                QString file = mpv->getPath()+f;
                if((recent.isEmpty() || recent.front() != file) &&
                   f != QString() &&
                   maxRecent > 0)
                {
                    UpdateRecentFiles(); // update after initialization and only if the current file is different from the first recent
                    recent.removeAll(file);
                    while(recent.length() > maxRecent-1)
                        recent.removeLast();
                    recent.push_front(file);
                }
            });

    connect(mpv, &MpvHandler::timeChanged,
            [=](int i)
            {
                const Mpv::FileInfo &fi = mpv->getFileInfo();
                // set the seekBar's location with NoSignal function so that it doesn't trigger a seek
                // the formula is a simple ratio seekBar's max * time/totalTime
                ui->seekBar->setValueNoSignal(ui->seekBar->maximum()*((double)i/fi.length));

                // set duration and remaining labels, QDateTime takes care of formatting for us
                ui->durationLabel->setText(FormatTime(i, mpv->getFileInfo().length));
                if(remaining)
                    ui->remainingLabel->setText("-"+FormatTime(fi.length-i, mpv->getFileInfo().length));

                // set next/previous chapter's enabled state
                if(fi.chapters.length() > 0)
                {
                    ui->action_Next_Chapter->setEnabled(i < fi.chapters.last().time);
                    ui->action_Previous_Chapter->setEnabled(i > fi.chapters.first().time);
                }
            });

    connect(mpv, &MpvHandler::volumeChanged,
            [=](int volume)
            {
                ui->volumeSlider->setValueNoSignal(volume);
            });

    connect(mpv, &MpvHandler::speedChanged,
            [=](double speed)
            {
                static double last = 1;
                if(last != speed)
                {
                    if(init)
                        mpv->ShowText(tr("Speed: %0x").arg(QString::number(speed)));
                    if(speed <= 0.25)
                        ui->action_Decrease->setEnabled(false);
                    else
                        ui->action_Decrease->setEnabled(true);
                    last = speed;
                }
            });

    connect(mpv, &MpvHandler::sidChanged,
            [=](int sid)
            {
                QList<QAction*> actions = ui->menuSubtitle_Track->actions();
                for(auto &action : actions)
                {
                    if(action->text().startsWith(QString::number(sid)))
                    {
                        action->setCheckable(true);
                        action->setChecked(true);
                    }
                    else
                        action->setChecked(false);
                }
            });

    connect(mpv, &MpvHandler::aidChanged,
            [=](int aid)
            {
                QList<QAction*> actions = ui->menuAudio_Tracks->actions();
                for(auto &action : actions)
                {
                    if(action->text().startsWith(QString::number(aid)))
                    {
                        action->setCheckable(true);
                        action->setChecked(true);
                    }
                    else
                        action->setChecked(false);
                }
            });

    connect(mpv, &MpvHandler::playlistVisibleChanged,
            [=](bool b)
            {
                ShowPlaylist(b);
            });

    connect(mpv, &MpvHandler::subtitleVisibilityChanged,
            [=](bool b)
            {
                ui->actionShow_Subtitles->setChecked(b);
            });

    connect(mpv, &MpvHandler::messageSignal,
            [=](QString msg)
            {
                ui->outputTextEdit->moveCursor(QTextCursor::End);
                ui->outputTextEdit->insertPlainText(msg);
            });

    // update manager

    /* automatic updating support
    connect(update, &UpdateManager::Update,
            [=](QMap<QString, QString> info)
            {
                if(info["version"] != BAKA_MPLAYER_VERSION)
                    update->DownloadUpdate();
            });

    connect(update, &UpdateManager::Downloaded,
            [=](int percent)
            {
                if(percent == 100)
                {
                    // prepare for update
                }
                // show progress as status message?
            });
    */

    // ui

    connect(ui->seekBar, &SeekBar::valueChanged,                        // Playback: Seekbar clicked
            [=](int i)
            {
                mpv->Seek(((double)i/ui->seekBar->maximum())*mpv->getFileInfo().length);
            });

    connect(ui->remainingLabel, &CustomLabel::clicked,                  // Playback: Remaining Label
            [=]
            {
                if(remaining)
                {
                    setRemaining(false);
                    ui->remainingLabel->setText(FormatTime(mpv->getFileInfo().length, mpv->getFileInfo().length));
                }
                else
                    setRemaining(true);
            });

    connect(ui->rewindButton, SIGNAL(clicked()),                        // Playback: Rewind button
            this, SLOT(HandleRewind()));

    connect(ui->previousButton, SIGNAL(clicked()),                      // Playback: Previous button
            this, SLOT(HandlePlayPrevious()));

    connect(ui->playButton, SIGNAL(clicked()),                          // Playback: Play/pause button
            this, SLOT(HandlePlayPause()));

    connect(ui->nextButton, SIGNAL(clicked()),                          // Playback: Next button
            this, SLOT(HandlePlayNext()));

    connect(ui->volumeSlider, &CustomSlider::valueChanged,              // Playback: Volume slider adjusted
            [=](int i)
            {
                mpv->Volume(i);
            });

    connect(ui->playlistButton, &QPushButton::clicked,                  // Playback: Clicked the playlist button
            [=]
            {
                ShowPlaylist(!ui->splitter->position());
            });

    connect(ui->splitter, &CustomSplitter::positionChanged,             // Splitter position changed
            [=](int i)
            {
                blockSignals(true);
                if(i == 0) // right-most, playlist is hidden
                {
                    ui->action_Show_Playlist->setChecked(false);
                    ui->action_Hide_Album_Art->setChecked(false);
                }
                else if(i == ui->splitter->max()) // left-most, album art is hidden, playlist is visible
                {
                    ui->action_Show_Playlist->setChecked(true);
                    ui->action_Hide_Album_Art->setChecked(true);
                }
                else // in the middle, album art is visible, playlist is visible
                {
                    ui->action_Show_Playlist->setChecked(true);
                    ui->action_Hide_Album_Art->setChecked(false);
                }
                blockSignals(false);
            });

    connect(ui->searchBox, &QLineEdit::textChanged,                     // Playlist: Search box
            [=](QString s)
            {
                ui->playlistWidget->Search(s);
            });

    connect(ui->indexLabel, &CustomLabel::clicked,                      // Playlist: Clicked the indexLabel
            [=]
            {
                QString res = InputDialog::getInput(tr("Enter the file number you want to play:\nNote: Value must be from 1 - %0").arg(QString::number(ui->playlistWidget->count())),
                                                    tr("Enter File Number"),
                                                    [this](QString input)
                                                    {
                                                        int in = input.toInt();
                                                        if(in >= 1 && in <= ui->playlistWidget->count())
                                                            return true;
                                                        return false;
                                                    },
                                                    this);
                if(res != "")
                    mpv->PlayFile(ui->playlistWidget->FileAt(res.toInt()-1)); // user index will be 1 greater than actual
            });

    connect(ui->playlistWidget, &PlaylistWidget::currentRowChanged,     // Playlist: Playlist selection changed
            [=](int i)
            {
                if(i == -1) // no selection
                {
                    ui->indexLabel->setText(tr("No selection"));
                    ui->indexLabel->setEnabled(false);
                }
                else
                {
                    ui->indexLabel->setEnabled(true);
                    ui->indexLabel->setText(tr("File %0 of %1").arg(QString::number(i+1), QString::number(ui->playlistWidget->count())));
                }
            });

    connect(ui->playlistWidget, &PlaylistWidget::doubleClicked,         // Playlist: Item double clicked
            [=](const QModelIndex &i)
            {
                mpv->PlayFile(ui->playlistWidget->FileAt(i.row()));
            });

    connect(ui->currentFileButton, &QPushButton::clicked,               // Playlist: Select current file button
            [=]
            {
                ui->playlistWidget->SelectItem(mpv->getFile());
            });

    connect(ui->hideFilesButton, &QPushButton::clicked,                 // Playlist: Hide files button
            [=](bool b)
            {
                ui->playlistWidget->ShowAll(!b);
            });

    connect(ui->refreshButton, &QPushButton::clicked,                   // Playlist: Refresh playlist button
            [=]
            {
                ui->playlistWidget->SelectItem(mpv->LoadPlaylist(mpv->getPath()+mpv->getFile()), true);
                ui->playlistWidget->ShowAll(!ui->hideFilesButton->isChecked());
                firstItem = false;
            });

    action = ui->playlistWidget->addAction(tr("R&emove from Playlist"));
    connect(action, &QAction::triggered,                                // Playlist: Remove from playlist (right-click)
            [=]
            {
                int row = ui->playlistWidget->currentRow();
                ui->playlistWidget->RemoveItem(row);
                if(row > 0)
                {
                    if(row < ui->playlistWidget->count()-1)
                        ui->playlistWidget->setCurrentRow(row);
                    else
                        ui->playlistWidget->setCurrentRow(row-1);
                }
            });

    action = ui->playlistWidget->addAction(tr("&Delete from Disk"));
    connect(action, &QAction::triggered,                                // Playlist: Delete from Disk (right-click)
            [=]
            {
                int row = ui->playlistWidget->currentRow();
                QString item = ui->playlistWidget->RemoveItem(row);
                if(row > 0)
                {
                    if(row < ui->playlistWidget->count()-1)
                        ui->playlistWidget->setCurrentRow(row);
                    else
                        ui->playlistWidget->setCurrentRow(row-1);
                }
                QFile f(mpv->getPath()+item);
                f.remove();
            });

    action = ui->playlistWidget->addAction(tr("&Refresh"));
    connect(action, &QAction::triggered,                                // Playlist: Refresh (right-click)
            [=]
            {
                ui->playlistWidget->SelectItem(mpv->LoadPlaylist(mpv->getPath()+mpv->getFile()), true);
                ui->playlistWidget->ShowAll(!ui->hideFilesButton->isChecked());
                firstItem = false;
            });

    connect(ui->inputLineEdit, &CustomLineEdit::submitted,
            [=](QString s)
            {
                ExecuteCommand(s);
                ui->inputLineEdit->setText("");
            });
                                                                        // File ->
    connect(ui->action_New_Player, SIGNAL(triggered()),                 // File -> New Player
            this, SLOT(HandleNewPlayer()));

    connect(ui->action_Open_File, SIGNAL(triggered()),                  // File -> Open File
            this, SLOT(HandleOpenFileDialog()));

    connect(ui->actionOpen_URL, SIGNAL(triggered()),                    // File -> Open URL
            this, SLOT(HandleOpenURLDialog()));

    connect(ui->actionOpen_Path_from_Clipboard, SIGNAL(triggered()),    // File -> Open Path from Clipboard
            this, SLOT(HandleOpenClipboard()));

    connect(ui->actionShow_in_Folder, SIGNAL(triggered()),              // File -> Show in Folder
            this, SLOT(HandleShowInFolder()));

    connect(ui->actionPlay_Next_File, SIGNAL(triggered()),              // File -> Play Next File
            this, SLOT(HandlePlayNext()));

    connect(ui->actionPlay_Previous_File, SIGNAL(triggered()),          // File -> Play Previous File
            this, SLOT(HandlePlayPrevious()));

    connect(ui->actionE_xit, SIGNAL(triggered()),                       // File -> Exit
            this, SLOT(HandleExit()));
                                                                        // View ->
    connect(ui->action_Full_Screen, &QAction::triggered,                // View -> Full Screen
            [=]
            {
                FullScreen(!isFullScreen());
            });

    connect(ui->actionWith_Subtitles, &QAction::triggered,              // View -> Take Screenshot -> With Subtitles
            [=]
            {
                TakeScreenshot(true);
            });

    connect(ui->actionWithout_Subtitles, &QAction::triggered,           // View -> Take Screenshot -> Without Subtitles
            [=]
            {
                TakeScreenshot(false);
            });
                                                                        // View -> Fit Window ->
    connect(ui->action_To_Current_Size, &QAction::triggered,            // View -> Fit Window -> To Current Size
            [=]
            {
                FitWindow(0);
            });

    connect(ui->action50, &QAction::triggered,                          // View -> Fit Window -> 50%
            [=]
            {
                FitWindow(50, true);
            });

    connect(ui->action75, &QAction::triggered,                          // View -> Fit Window -> 75%
            [=]
            {
                FitWindow(75, true);
            });

    connect(ui->action100, &QAction::triggered,                         // View -> Fit Window -> 100%
            [=]
            {
                FitWindow(100, true);
            });

    connect(ui->action200, &QAction::triggered,                         // View -> Fit Window -> 200%
            [=]
            {
                FitWindow(200, true);
            });
                                                                        // View -> Aspect Ratio ->
    connect(ui->action_Auto_Detect, &QAction::triggered,                // View -> Aspect Ratio -> Auto Detect
            [=]
            {
                SetAspectRatio("-1");
            });

    connect(ui->actionForce_4_3, &QAction::triggered,                   // View -> Aspect Ratio -> 4:3
            [=]
            {
                SetAspectRatio("4:3");
            });

    connect(ui->actionForce_2_35_1, &QAction::triggered,                // View -> Aspect Ratio -> 2.35:1
            [=]
            {
                SetAspectRatio("2_35:1");
            });

    connect(ui->actionForce_16_9, &QAction::triggered,                  // View -> Aspect Ratio -> 16:9
            [=]
            {
                SetAspectRatio("16:9");
            });

    connect(ui->actionShow_Subtitles, &QAction::triggered,              // View -> Show Subtitles
            [=](bool b)
            {
                mpv->ShowSubtitles(b);
            });

    connect(ui->action_Add_Subtitle_File, &QAction::triggered,          //  View -> Subtitle Track -> Add Subtitle File...
            [=]
            {
                QString trackFile = QFileDialog::getOpenFileName(this, tr("Open Subtitle File"), mpv->getPath(),
                                                                 tr("Subtitle Files (%0)").arg(Mpv::subtitle_filetypes.join(" ")),
                                                                 0, QFileDialog::DontUseSheet);
                if(trackFile != "")
                    mpv->AddSubtitleTrack(trackFile);
            });
                                                                        // View -> Font Size ->
    connect(ui->action_Size, &QAction::triggered,                       // View -> Font Size -> Size +
            [=]
            {
                mpv->SubtitleScale(.02, true);
            });

    connect(ui->actionS_ize, &QAction::triggered,                       // View -> Font Size -> Size -
            [=]
            {
                mpv->SubtitleScale(-.02, true);
            });

    connect(ui->action_Reset_Size, &QAction::triggered,                 // View -> Font Size -> Reset Size
            [=]
            {
                mpv->SubtitleScale(1);
            });

    connect(ui->actionMedia_Info, &QAction::triggered,                  // View -> Media Info
            [=]
            {
                InfoDialog::info(mpv->getPath()+mpv->getFile(), mpv->getFileInfo(), this);
            });
                                                                        // Playback ->
    connect(ui->action_Play, &QAction::triggered,                       // Playback -> (Play|Pause)
            [=]
            {
                mpv->PlayPause(ui->playlistWidget->CurrentItem());
            });

    connect(ui->action_Stop, &QAction::triggered,                       // Playback -> Stop
            [=]
            {
                mpv->Stop();
            });

    connect(ui->action_Restart, &QAction::triggered,                    // Playback -> Restart
            [=]
            {
                mpv->Restart();
            });
                                                                        // Playback -> Speed ->
    connect(ui->action_Increase, &QAction::triggered,                   // Playback -> Speed -> Increase
            [=]
            {
                mpv->Speed(mpv->getSpeed()+.25);
            });

    connect(ui->action_Decrease, &QAction::triggered,                   // Playback -> Speed -> Increase
            [=]
            {
                mpv->Speed(mpv->getSpeed()-.25);
            });

    connect(ui->action_Reset, &QAction::triggered,                      // Playback -> Speed -> Reset
            [=]
            {
                mpv->Speed(1);
            });

    connect(ui->actionSh_uffle, &QAction::triggered,                    // Playback -> Shuffle
            [=](bool b)
            {
                ui->playlistWidget->Shuffle(b);
            });
                                                                        // Playback -> Repeat
    connect(ui->action_Off, &QAction::triggered,                        // Playback -> Repeat -> Off
            [=](bool b)
            {
                if(b)
                {
                    ui->action_This_File->setChecked(false);
                    ui->action_Playlist->setChecked(false);
                }
            });

    connect(ui->action_This_File, &QAction::triggered,                  // Playback -> Repeat -> This File
            [=](bool b)
            {
                if(b)
                {
                    ui->action_Off->setChecked(false);
                    ui->action_Playlist->setChecked(false);
                }
            });

    connect(ui->action_Playlist, &QAction::triggered,                   // Playback -> Repeat -> Playlist
            [=](bool b)
            {
                if(b)
                {
                    ui->action_Off->setChecked(false);
                    ui->action_This_File->setChecked(false);
                }
            });

    connect(ui->action_Increase_Volume, &QAction::triggered,            // Playback -> Increase Volume
            [=]
            {
                mpv->Volume(mpv->getVolume()+5);
            });

    connect(ui->action_Decrease_Volume, &QAction::triggered,            // Playback -> Decrease Volume
            [=]
            {
                mpv->Volume(mpv->getVolume()-5);
            });
                                                                        // Navigate ->
    connect(ui->action_Next_Chapter, &QAction::triggered,               // Navigate -> Next Chapter
            [=]
            {
                mpv->NextChapter();
            });

    connect(ui->action_Previous_Chapter, &QAction::triggered,           // Navigate -> Previous Chapter
            [=]
            {
                mpv->PreviousChapter();
            });

    connect(ui->action_Frame_Step, &QAction::triggered,                 // Navigate -> Frame Step
            [=]
            {
                mpv->FrameStep();
            });

    connect(ui->actionFrame_Back_Step, &QAction::triggered,             // Navigate -> Frame Back Step
            [=]
            {
                mpv->FrameBackStep();
            });

    connect(ui->action_Jump_to_Time, &QAction::triggered,               // Navigate -> Jump to Time
            [=]
            {
                int time = JumpDialog::getTime(mpv->getFileInfo().length,this);
                if(time >= 0)
                    mpv->Seek(time);
            });
                                                                        // Settings ->
    connect(ui->action_Show_Playlist, &QAction::triggered,              // Settings -> Show Playlist
            [=](bool b)
            {
                ShowPlaylist(b);
            });

    connect(ui->action_Hide_Album_Art, &QAction::triggered,             // Settings -> Hide Album Art
            [=](bool b)
            {
                HideAlbumArt(b);
            });

    connect(ui->action_Dim_Lights, &QAction::triggered,                 // Settings -> Dim Lights
            [=](bool b)
            {
                DimLights(b);
            });

    connect(ui->actionShow_D_ebug_Output, SIGNAL(triggered()),          // Settings -> Show Debug Output
            this, SLOT(setDebug(bool)));

    connect(ui->action_Preferences, SIGNAL(triggered()),                // Settings -> Preferences...
            this, SLOT(HandlePreferencesDialog()));
                                                                        // Help ->
    connect(ui->actionOnline_Help, SIGNAL(triggered()),                 // Help -> Online Help
            this, SLOT(HandleOnlineHelp()));

    connect(ui->action_Check_for_Updates, SIGNAL(triggered()),          // Help -> Check for Updates
            this, SLOT(HandleUpdateDialog()));

    connect(ui->actionAbout_Qt, SIGNAL(triggered()),                    // Help -> About Qt
            this, SLOT(HandleAboutQtDialog()));

    connect(ui->actionAbout_Baka_MPlayer, SIGNAL(triggered()),          // Help -> About Baka MPlayer
            this, SLOT(HandleAboutDialog()));

    // qApp

    if(dimDialog) // no need to monitor focus if you can't use dimDialog
    {
        connect(qApp, &QApplication::focusWindowChanged,
                [=](QWindow *focusWindow)
                {
                    // note: focusWindow will be 0 if anything is clicked outside of our program which is useful
                    // the only other problem is that when dragging by the top handle
                    // it will be 0 thus reverting dim lights, this is a side effect
                    // which will have to stay for now.
                    if(dimDialog->isVisible())
                    {
                        if(focusWindow == 0)
                        {
                            dimDialog->setVisible(false); // remove dim lights
                            ui->action_Dim_Lights->setChecked(false); // uncheck dim lights
                        }
                        else if(focusWindow == dimDialog->windowHandle())
                        {
                            activateWindow();
                            raise();
                            setFocus();
                        }
                    }
                });
        connect(dimDialog, &DimDialog::clicked,
                [=]
                {
                    dimDialog->setVisible(false); // remove dim lights
                    ui->action_Dim_Lights->setChecked(false); // uncheck dim lights
                    activateWindow();
                    raise();
                    setFocus();
                });
    }
    // set window geometry from settings: leave this out of settings so that preference dialog doesn't center/resize the window
    setGeometry(QStyle::alignedRect(Qt::LeftToRight,
                                    Qt::AlignCenter,
                                    QSize(settings->value("baka-mplayer/width", 600).toInt(),
                                          settings->value("baka-mplayer/height", 430).toInt()),
                                    qApp->desktop()->availableGeometry()));
}

MainWindow::~MainWindow()
{
    SaveSettings();

    // note: child objects do not need to be deleted;
    // all children get deleted when mainwindow is deleted
    // see: http://qt-project.org/doc/qt-4.8/objecttrees.html
    if(dimDialog)
        delete dimDialog;
    if(moveTimer)
        delete moveTimer;
    delete ui;
}

void MainWindow::Load(QString file)
{
    // load the settings here--the constructor has already been called
    // this solves some issues with setting things before the constructor has ended
    menuVisible = ui->menubar->isVisible(); // does the OS use a menubar? (appmenu doesn't)
    LoadSettings();
    mpv->LoadFile(file);
}

void MainWindow::LoadSettings()
{
    if(settings)
    {
        QString version;
        if(settings->allKeys().length() == 0) // empty settings
        {
            version = "2.0.2"; // current version
            WriteDefaultKeybindings();
        }
        else
            version = settings->value("baka-mplayer/version", "1.9.9").toString(); // defaults to the first version without version info in settings

        if(version == "2.0.2") // current version
        {
            settings->beginGroup("baka-mplayer");
            setOnTop(settings->value("onTop", "never").toString());
            setAutoFit(settings->value("autoFit", 100).toInt());
            sysTrayIcon->setVisible(settings->value("trayIcon", false).toBool());
            setHidePopup(settings->value("hidePopup", false).toBool());
            setRemaining(settings->value("remaining", true).toBool());
            ui->splitter->setNormalPosition(settings->value("splitter", ui->splitter->max()*1.0/8).toInt());
            setDebug(settings->value("debug", false).toBool());
            ui->hideFilesButton->setChecked(!settings->value("showAll", true).toBool());
            setScreenshotDialog(settings->value("screenshotDialog", true).toBool());
            recent = settings->value("recent").toStringList();
            maxRecent = settings->value("maxRecent", 5).toInt();
            settings->endGroup();
            UpdateRecentFiles();
            LoadKeybindings();

            mpv->LoadSettings(settings, version);
        }
        else if(version == "2.0.1")
        {
            settings->beginGroup("baka-mplayer");
            setOnTop(settings->value("onTop", "never").toString());
            setAutoFit(settings->value("autoFit", 100).toInt());
            sysTrayIcon->setVisible(settings->value("trayIcon", false).toBool());
            setHidePopup(settings->value("hidePopup", false).toBool());
            setRemaining(settings->value("remaining", true).toBool());
            ui->splitter->setNormalPosition(settings->value("splitter", ui->splitter->max()*1.0/8).toInt());
            setDebug(settings->value("debug", false).toBool());
            ui->hideFilesButton->setChecked(!settings->value("showAll", true).toBool());
            setScreenshotDialog(settings->value("screenshotDialog", true).toBool());
            recent = settings->value("recent").toStringList();
            maxRecent = settings->value("maxRecent", 5).toInt();
            settings->endGroup();
            UpdateRecentFiles();

            mpv->LoadSettings(settings, version);

            settings->clear();
            SaveSettings();
            WriteDefaultKeybindings();
            LoadKeybindings();
        }
        else if(version == "2.0.0")
        {
            settings->beginGroup("baka-mplayer");
            setOnTop(settings->value("onTop", "never").toString());
            setAutoFit(settings->value("autoFit", 100).toInt());
            sysTrayIcon->setVisible(settings->value("trayIcon", false).toBool());
            setHidePopup(settings->value("hidePopup", false).toBool());
            setRemaining(settings->value("remaining", true).toBool());
            ui->splitter->setNormalPosition(settings->value("splitter", ui->splitter->max()*1.0/8).toInt());
            setDebug(settings->value("debug", false).toBool());
            ui->hideFilesButton->setChecked(!settings->value("showAll", true).toBool());
            setScreenshotDialog(settings->value("screenshotDialog", true).toBool());
            maxRecent = 5;
            QString lf = settings->value("lastFile").toString();
            if(lf != QString())
                recent.push_front(lf);
            settings->endGroup();
            UpdateRecentFiles();

            mpv->LoadSettings(settings, version);

            settings->clear();
            SaveSettings();
            WriteDefaultKeybindings();
            LoadKeybindings();
        }
        else if(version == "1.9.9")
        {
            settings->beginGroup("window");
            setGeometry(QStyle::alignedRect(Qt::LeftToRight,
                                            Qt::AlignCenter,
                                            QSize(settings->value("width", 600).toInt(),
                                                  settings->value("height", 430).toInt()),
                                            qApp->desktop()->availableGeometry()));
            setOnTop(settings->value("onTop", "never").toString());
            setAutoFit(settings->value("autoFit", 100).toInt());
            sysTrayIcon->setVisible(settings->value("trayIcon", false).toBool());
            setHidePopup(settings->value("hidePopup", false).toBool());
            setRemaining(settings->value("remaining", true).toBool());
            ui->splitter->setNormalPosition(settings->value("splitter", ui->splitter->max()*1.0/8).toInt());
            ui->hideFilesButton->setChecked(!settings->value("showAll", true).toBool());
            settings->endGroup();
            setDebug(settings->value("common/debug", false).toBool());
            maxRecent = 5;
            setScreenshotDialog(true);
            QString lf = settings->value("mpv/lastFile").toString();
            if(lf != QString())
                recent.push_front(lf);
            UpdateRecentFiles();

            mpv->LoadSettings(settings, version);

            settings->clear();
            SaveSettings();
            WriteDefaultKeybindings();
            LoadKeybindings();
        }
        else // unrecognized version (newer)
        {
            version = "2.0.2"; // load what we can assuming the settings are like the current version

            settings->beginGroup("baka-mplayer");
            setOnTop(settings->value("onTop", "never").toString());
            setAutoFit(settings->value("autoFit", 100).toInt());
            sysTrayIcon->setVisible(settings->value("trayIcon", false).toBool());
            setHidePopup(settings->value("hidePopup", false).toBool());
            setRemaining(settings->value("remaining", true).toBool());
            ui->splitter->setNormalPosition(settings->value("splitter", ui->splitter->max()*1.0/8).toInt());
            setDebug(settings->value("debug", false).toBool());
            ui->hideFilesButton->setChecked(!settings->value("showAll", true).toBool());
            setScreenshotDialog(settings->value("screenshotDialog", true).toBool());
            recent = settings->value("recent").toStringList();
            maxRecent = settings->value("maxRecent", 5).toInt();
            settings->endGroup();
            UpdateRecentFiles();
            LoadKeybindings();

            mpv->LoadSettings(settings, version);

            // disable settings manipulation
            delete settings;
            settings = 0;
            ui->action_Preferences->setEnabled(false);

            QMessageBox::information(this, "Settings version not recognized", "The settings file was made by a newer version of baka-mplayer; please upgrade this version or seek assistance from the developers.\nSome features may not work and changed settings will not be saved.");
        }
    }
}

void MainWindow::SaveSettings()
{
    if(settings)
    {
        // mpv
        mpv->SaveSettings(settings);

        settings->beginGroup("baka-mplayer");
        settings->setValue("width", normalGeometry().width());
        settings->setValue("height", normalGeometry().height());
        settings->setValue("onTop", onTop);
        settings->setValue("autoFit", autoFit);
        settings->setValue("trayIcon", sysTrayIcon->isVisible());
        settings->setValue("hidePopup", hidePopup);
        settings->setValue("remaining", remaining);
        settings->setValue("splitter", (ui->splitter->position() == 0 ||
                                        ui->splitter->position() == ui->splitter->max()) ?
                                        ui->splitter->normalPosition() :
                                        ui->splitter->position());
        settings->setValue("showAll", !ui->hideFilesButton->isChecked());
        settings->setValue("screenshotDialog", screenshotDialog);
        settings->setValue("debug", debug);
        settings->setValue("recent", recent);
        settings->setValue("maxRecent", maxRecent);
        settings->setValue("version", "2.0.2");
        settings->endGroup();
    }
}

void MainWindow::LoadKeybindings()
{
    settings->beginGroup("input");
    QStringList keys = settings->allKeys();
    QString command;
    for(auto &key : keys)
    {
        command = settings->value(key).toString();
        if(key == "OpenLeftClick")
        {
            connect(ui->openButton, &OpenButton::LeftClick,
                    [=] { ExecuteCommand(command); });
        }
        else if(key == "OpenMiddleClick")
        {
            connect(ui->openButton, &OpenButton::MiddleClick,
                    [=] { ExecuteCommand(command); });
        }
        else if(key == "OpenRightClick")
        {
            connect(ui->openButton, &OpenButton::RightClick,
                    [=] { ExecuteCommand(command); });
        }
        else if(key == "ScrollUp")
        {
            // TODO
        }
        else if(key == "ScrollDown")
        {
            // TODO
        }
        else if(key == "FrameLeftClick" ||
                key == "FrameDoubleClick" ||
                key == "FrameRightClick" ||
                key == "FrameMiddleClick")
        {
            input[key] = command;
        }
        else
        {
            QAction *action = new QAction(this);
            action->setShortcut(QKeySequence(key));
            connect(action, &QAction::triggered,
                    [=] { ExecuteCommand(command); });
            addAction(action);
        }
    }
    settings->endGroup();
}

void MainWindow::WriteDefaultKeybindings()
{
    QVector<QPair<QString, QString>> keybindings = {
        {"OpenLeftClick", "baka open_file_dialog"},
        {"OpenMiddleClick", "baka jump_dialog"},
        {"OpenRightClick", "baka open_url_dialog"},
        {"ScrollUp", "mpv add volume 1"},
        {"ScrollDown", "mpv add volume -1"},
        {"FrameLeftClick", "baka move"},
        {"FrameDoubleClick", "baka toggle fullscreen"},
        {"FrameRightClick", "baka play_pause"},
        {"Ctrl+N", "baka new_player"},
        {"Ctrl+O", "baka open_file_dialog"},
        {"Ctrl+U", "baka open_url_dialog"},
        {"Ctrl+V", "baka open_clipboard"},
        {"Ctrl+Z", "baka open_recent 0"},
        {"Ctrl+F", "baka show_in_folder"},
        {"Ctrl+Left", "baka play_next"},
        {"Ctrl+Right", "baka play_previous"},
        {"Ctrl+Q", "baka exit"},
        {"Alt+Return", "baka toggle fullscreen"},
        {"Ctrl+W", "mpv toggle sub-visibility"},
        {"Ctrl+I", "baka media_info_dialog"},
        {"Space", "baka play_pause"},
        {"Ctrl+S", "baka stop"},
        {"Ctrl+R", "mpv set time-pos 0"},
        {"PgDown", "mpv add chapter 1"},
        {"PgUp", "mpv add chapter -1"},
        {"Shift+Right", "mpv frame_step"},
        {"Shift+Left", "mpv frame_back_step"},
        {"Ctrl+J", "baka jump_dialog"},
        {"Ctrl+X", "baka toggle playlist"},
        {"Ctrl+D", "baka toggle dim_lights"},
        {"F1", "baka online_help"},
        {"Alt+1", "baka auto_fit 0"},
        {"Alt+2", "baka auto_fit 50"},
        {"Alt+3", "baka auto_fit 75"},
        {"Alt+4", "baka auto_fit 100"},
        {"Alt+5", "baka auto_fit 200"},
        {"Ctrl++", "mpv add sub-scale 0.02"},
        {"Ctrl+-", "mpv add sub-scale -0.02"},
        {"Ctrl+E", "baka toggle debug"},
        {"Ctrl+Up", "mpv add volume 5"},
        {"Ctrl+Down", "mpv add volume -5"},
        {"Ctrl+T", "mpv screenshot subtitles"},
        {"Ctrl+Shift+T", "mpv screenshot video"},
        {"Ctrl+Shift+Up", "mpv add speed 0.25"},
        {"Ctrl+Shift+Down", "mpv add speed -0.25"},
        {"Ctrl+Shift+R", "mpv set speed 1"},
        {"Left", "mpv seek 5"},
        {"Right", "mpv seek -5"},
        {"Esc", "baka boss"}
    };

    settings->beginGroup("input");
    for(auto &binding : keybindings)
        settings->setValue(binding.first, binding.second);
    settings->endGroup();
}

void MainWindow::dragEnterEvent(QDragEnterEvent *event)
{
    if(event->mimeData()->hasUrls() || event->mimeData()->hasText()) // url / text
        event->acceptProposedAction();
}

void MainWindow::dropEvent(QDropEvent *event)
{
    const QMimeData *mimeData = event->mimeData();
    if(mimeData->hasUrls()) // urls
    {
        for(QUrl &url : mimeData->urls())
        {
            if(url.isLocalFile())
                mpv->LoadFile(url.toLocalFile());
            else
                mpv->LoadFile(url.url());
        }
    }
    else if(mimeData->hasText()) // text
        mpv->LoadFile(mimeData->text());
}

void MainWindow::mousePressEvent(QMouseEvent *event)
{
    if(ui->mpvFrame->rect().contains(event->pos())) // mouse is in the mpvFrame
    {
        lastMousePos = event->globalPos();
        if(event->button() == Qt::LeftButton)
            ExecuteCommand(input["FrameLeftClick"]);
        else if(event->button() == Qt::RightButton)
            ExecuteCommand(input["FrameRightClick"]);
        else if(event->button() == Qt::MiddleButton)
            ExecuteCommand(input["FrameMiddleClick"]);
    }
    QMainWindow::mousePressEvent(event);
}

void MainWindow::mouseReleaseEvent(QMouseEvent *event)
{
    if(moveTimer)
    {
        delete moveTimer;
        moveTimer = nullptr;
    }
    QMainWindow::mouseReleaseEvent(event);
}

void MainWindow::mouseMoveEvent(QMouseEvent *event)
{
    static QRect playbackRect;

    if(moveTimer && moveTimer->elapsed() > 10)
    {
        QMainWindow::move(origPos+event->globalPos()-lastMousePos);
        event->accept();
        moveTimer->restart();
    }
    else if(isFullScreen())
    {
        setCursor(QCursor(Qt::ArrowCursor)); // show the cursor
        autohide->stop();

        if(!ui->playbackLayoutWidget->isVisible())
        {
            if(playbackRect.contains(event->pos()))
            {
                ui->playbackLayoutWidget->setVisible(true);
                ui->seekBar->setVisible(true);
            }
            else
                autohide->start(500);
        }
        else
        {
            playbackRect = ui->playbackLayoutWidget->geometry();
            playbackRect.setTop(playbackRect.top()-20);
            if(!playbackRect.contains(event->pos()))
            {
                ui->playbackLayoutWidget->setVisible(false);
                ui->seekBar->setVisible(false);
                autohide->start(500);
            }
        }
    }
    QMainWindow::mouseMoveEvent(event);
}

bool MainWindow::eventFilter(QObject *obj, QEvent *event)
{
    if(obj == ui->mpvFrame && event->type() == QEvent::MouseMove && isFullScreen())
    {
        QMouseEvent *mouseEvent = static_cast<QMouseEvent*>(event);
        mouseMoveEvent(mouseEvent);
    }
    return false;
}

void MainWindow::mouseDoubleClickEvent(QMouseEvent *event)
{
    if(event->button() == Qt::LeftButton && ui->mpvFrame->geometry().contains(event->pos())) // if mouse is in the mpvFrame
    {
        ExecuteCommand(input["FrameDoubleClick"]);
        event->accept();
    }
    QMainWindow::mouseDoubleClickEvent(event);
}

void MainWindow::SetPlaybackControls(bool enable)
{
    // playback controls
    ui->seekBar->setEnabled(enable);
    ui->rewindButton->setEnabled(enable);
    // next file
    if(enable && ui->playlistWidget->currentRow()+1 < ui->playlistWidget->count()) // not the last entry
    {
        ui->nextButton->setEnabled(true);
        ui->nextButton->setIndex(ui->playlistWidget->currentRow()+2); // starting at 1 instead of at 0 like actual index
        ui->actionPlay_Next_File->setEnabled(true);
    }
    else
    {
        ui->nextButton->setEnabled(false);
        ui->actionPlay_Next_File->setEnabled(false);
    }
    // previous file
    if(enable && ui->playlistWidget->currentRow()-1 >= 0) // not the first entry
    {
        ui->previousButton->setEnabled(true);
        ui->previousButton->setIndex(-ui->playlistWidget->currentRow()); // we use a negative index value for the left button
        ui->actionPlay_Previous_File->setEnabled(true);
    }
    else
    {
        ui->previousButton->setEnabled(false);
        ui->actionPlay_Previous_File->setEnabled(false);
    }
    // menubar
    ui->action_Stop->setEnabled(enable);
    ui->action_Restart->setEnabled(enable);
    ui->menuS_peed->setEnabled(enable);
    ui->action_Jump_to_Time->setEnabled(enable);
    ui->actionMedia_Info->setEnabled(enable);
    ui->actionShow_in_Folder->setEnabled(enable);
    ui->action_Full_Screen->setEnabled(enable);
    if(!enable)
    {
        ui->action_Hide_Album_Art->setEnabled(false);
        ui->menuSubtitle_Track->setEnabled(false);
        ui->menuAudio_Tracks->setEnabled(false);
        ui->menuFont_Si_ze->setEnabled(false);
    }
}

void MainWindow::FullScreen(bool fs)
{
    if(fs)
    {
        if(dimDialog && dimDialog->isVisible())
        {
            dimDialog->setVisible(false);
            ui->action_Dim_Lights->setChecked(false);
        }
        setWindowState(windowState() | Qt::WindowFullScreen);
        ui->menubar->setVisible(false);
        ShowPlaylist(false);
        setMouseTracking(true); // register mouse move event
        setContextMenuPolicy(Qt::ActionsContextMenu);
        // post a mouseMoveEvent (in case user doesn't actually move the mouse when entering fs)
        QCoreApplication::postEvent(this, new QMouseEvent(QMouseEvent::MouseMove,
                                                          QCursor::pos(),
                                                          Qt::NoButton,Qt::NoButton,Qt::NoModifier));
    }
    else
    {
        setWindowState(windowState() & ~Qt::WindowFullScreen);
        if(menuVisible)
            ui->menubar->setVisible(true);
        ui->seekBar->setVisible(true);
        ui->playbackLayoutWidget->setVisible(true);
        setMouseTracking(false); // stop registering mouse move event
        setContextMenuPolicy(Qt::NoContextMenu);
        setCursor(QCursor(Qt::ArrowCursor)); // show cursor
        autohide->stop();
    }
}

void MainWindow::ShowPlaylist(bool visible)
{
    if(visible)
        ui->splitter->setPosition(ui->splitter->normalPosition()); // bring splitter position to normal
    else
    {
        if(ui->splitter->position() != ui->splitter->max() && ui->splitter->position() != 0)
            ui->splitter->setNormalPosition(ui->splitter->position()); // save current splitter position as the normal position
        ui->splitter->setPosition(0); // set splitter position to right-most
    }
}

void MainWindow::HideAlbumArt(bool hide)
{
    if(hide)
    {
        if(ui->splitter->position() != ui->splitter->max() && ui->splitter->position() != 0)
            ui->splitter->setNormalPosition(ui->splitter->position()); // save splitter position as the normal position
        ui->splitter->setPosition(ui->splitter->max()); // bring the splitter position to the left-most
    }
    else
        ui->splitter->setPosition(ui->splitter->normalPosition()); // bring the splitter to normal position
}

void MainWindow::FitWindow(int percent, bool msg)
{
    if(isFullScreen() || isMaximized())
        return;

    mpv->LoadVideoParams();
    const Mpv::VideoParams &params = mpv->getFileInfo().video_params;
    QRect fG = ui->mpvFrame->geometry(), // frame geometry
          cG = geometry(), // current geometry of window
          dG = qApp->desktop()->availableGeometry(); // desktop geometry
    int w, h;
    double a;

    // get aspect ratio
    if(params.width == 0 || params.height == 0) // width/height are 0 when there is no output
        return;
    if(params.dwidth == 0 || params.dheight == 0) // dwidth/height are 0 on load
        a = (double)params.width/params.height; // use video width and height for aspect ratio
    else
        a = (double)params.dwidth/params.dheight; // use display geometry for aspect ratio

    // get width and height of new display
    if(percent == 0) // fit to window
    {
        w = fG.width();
        h = fG.height();
        dG = cG; // mascarade the desktop geometry so that it centers in-place
    }
    else
    {
        double scale = percent/100.0;
        w = params.width*scale;
        h = (params.width/a)*scale; // get height from aspect ratio

        // bigger than desktop geometry correction
        // todo: explain how this works, I came up with the algorithm and
        // simplified but intuitively it's hard to understand
        if(w + (frameGeometry().width() - fG.width()) > dG.width())
        {
            w = dG.width() - frameGeometry().width() + fG.width();
            h = w/a;
        }
        if(h + (frameGeometry().height() - fG.height()) > dG.height())
        {
            h = dG.height() - frameGeometry().height() + fG.height();
            w = a*h;
        }
    }

    // autofit algorithm
    if((double)w/h > a) // width > what it's supposed to be
        w = a*h;
    else                // height > what it's supposed to be
        h = w/a;

    // add the size of the things not in the frame
    w += cG.width() - fG.width();
    h += cG.height() - fG.height();

    // set window position
    setGeometry(QStyle::alignedRect(Qt::LeftToRight,
                                    Qt::AlignCenter,
                                    QSize(w, h),
                                    dG));
    if(msg)
        mpv->ShowText("Fit Window: "+QString::number(percent)+"%");
}

void MainWindow::SetAspectRatio(QString aspect)
{
    if(isFullScreen())
        return;
    mpv->Aspect(aspect);
}

void MainWindow::DimLights(bool dim)
{
    if(!dimDialog) // dimDialog is NULL if desktop compositor is disabled or missing
    {
        QMessageBox::information(this, tr("Dim Lights"), tr("In order to dim the lights, the desktop compositor has to be enabled. This can be done through Window Manager Desktop."));
        ui->action_Dim_Lights->setChecked(false);
        return;
    }
    if(dim)
        dimDialog->show();
    else
        dimDialog->close();
}

void MainWindow::AlwaysOnTop(bool ontop)
{
    // maybe in the future, Linux X specific code that way we could enable it for both platforms
#if defined(Q_OS_WIN)
    SetWindowPos((HWND)winId(),
                 ontop ? HWND_TOPMOST : HWND_NOTOPMOST,
                 0, 0, 0, 0,
                 SWP_NOSIZE | SWP_NOMOVE | SWP_SHOWWINDOW);
#elif defined(Q_OS_LINUX)
    Display *display = QX11Info::display();
    XEvent event;
    event.xclient.type = ClientMessage;
    event.xclient.serial = 0;
    event.xclient.send_event = True;
    event.xclient.display = display;
    event.xclient.window  = winId();
    event.xclient.message_type = XInternAtom (display, "_NET_WM_STATE", False);
    event.xclient.format = 32;

    event.xclient.data.l[0] = ontop;
    event.xclient.data.l[1] = XInternAtom (display, "_NET_WM_STATE_ABOVE", False);
    event.xclient.data.l[2] = 0; //unused.
    event.xclient.data.l[3] = 0;
    event.xclient.data.l[4] = 0;

    XSendEvent(display, DefaultRootWindow(display), False,
                           SubstructureRedirectMask|SubstructureNotifyMask, &event);
#else // qt code
    if(ontop)
        setWindowFlags(windowFlags() | Qt::WindowStaysOnTopHint);
    else
        setWindowFlags(windowFlags() & ~Qt::WindowStaysOnTopHint);
    show();
#endif
}

void MainWindow::TakeScreenshot(bool subs)
{
    if(screenshotDialog)
    {
        mpv->Pause();
        if(ScreenshotDialog::showScreenshotDialog(screenshotDialog, subs, mpv) != QDialog::Accepted)
            return;
    }
    else
        mpv->Screenshot(subs);
    ShowScreenshotMessage(subs);
}

void MainWindow::ShowScreenshotMessage(bool subs)
{
    QString dir = mpv->getScreenshotDir();
    int i = dir.lastIndexOf('/');
    if(i != -1)
        dir.remove(0, i+1);
    if(subs)
        mpv->ShowText(tr("Saved to \"%0\", with subs").arg(dir));
    else
        mpv->ShowText(tr("Saved to \"%0\", without subs").arg(dir));
}

void MainWindow::UpdateRecentFiles()
{
    ui->menu_Recently_Opened->clear();
    QAction *action;
    int n = 0,
        N = recent.length();
    for(auto &f : recent)
    {
        action = ui->menu_Recently_Opened->addAction(tr("%0. %1").arg(FormatNumberWithAmpersand(++n, N), ShortenPathToParent(f).replace("&","&&")));
        connect(action, &QAction::triggered,
                [=]
                {
                    mpv->LoadFile(f);
                });
    }
}

void MainWindow::OpenRecent(int index)
{
    if(recent.length() > index)
        mpv->LoadFile(recent[index]);
}

void MainWindow::BakaOutput(QString output)
{
    ui->outputTextEdit->moveCursor(QTextCursor::End);
    ui->outputTextEdit->insertPlainText(tr("[baka]: %0").arg(output));
}

void MainWindow::ExecuteCommand(QString command)
{
    if(command == QString())
        return;

    QStringList cmdList = command.split(" ");
    if(cmdList[0] == "baka")
    {
        cmdList.pop_front();
        HandleBakaCommand(cmdList);
    }
    else if(cmdList[0] == "mpv")
    {
        cmdList.pop_front();
        mpv->CommandString(cmdList.join(" "));
    }
    else
        HandleInvalidCommand(command);
}

void MainWindow::HandleBakaCommand(QStringList cmdList)
{
    if(cmdList.length() == 1)
    {
        if(cmdList[0] == "play_pause")                      HandlePlayPause();
        else if(cmdList[0] == "play_next")                  HandlePlayNext();
        else if(cmdList[0] == "play_previous")              HandlePlayPrevious();
        else if(cmdList[0] == "rewind")                     HandleRewind();
        else if(cmdList[0] == "stop")                       HandleStop();
        else if(cmdList[0] == "move")                       HandleMove();
        else if(cmdList[0] == "boss")                       HandleBoss();
        else if(cmdList[0] == "open_file_dialog")           HandleOpenFileDialog();
        else if(cmdList[0] == "open_url_dialog")            HandleOpenURLDialog();
        else if(cmdList[0] == "open_clipboard")             HandleOpenClipboard();
        else if(cmdList[0] == "jump_dialog")                HandleJumpDialog();
        else if(cmdList[0] == "show_in_folder")             HandleShowInFolder();
        else if(cmdList[0] == "new_player")                 HandleNewPlayer();
        else if(cmdList[0] == "media_info_dialog")          HandleMediaInfoDialog();
        else if(cmdList[0] == "media_preferences_dialog")   HandlePreferencesDialog();
        else if(cmdList[0] == "media_update_dialog")        HandleUpdateDialog();
        else if(cmdList[0] == "online_help")                HandleOnlineHelp();
        else if(cmdList[0] == "about_dialog")               HandleAboutDialog();
        else if(cmdList[0] == "about_qt_dialog")            HandleAboutQtDialog();
        else if(cmdList[0] == "exit")                       HandleExit();
        else                                                HandleInvalidCommand(cmdList[0]);
    }
    else if(cmdList.length() == 2)
    {
        if(cmdList[0] == "toggle")
        {
            if(cmdList[1] == "fullscreen")                  FullScreen(!isFullScreen());
            else if(cmdList[1] == "playlist")               ShowPlaylist(!ui->splitter->position());
            else if(cmdList[1] == "dim_lights")             DimLights(!dimDialog->isVisible());
            else if(cmdList[1] == "debug")                  setDebug(!debug);
            else                                            HandleInvalidParameter(cmdList[1]);
        }
        else if(cmdList[0] == "get")
        {
            if(cmdList[1] == "fullscreen")                  BakaOutput(QString::number(isFullScreen()));
            else if(cmdList[1] == "playlist")               BakaOutput(QString::number(ui->splitter->position()!=0));
            else if(cmdList[1] == "dim_lights")             BakaOutput(QString::number(dimDialog->isVisible()));
            else if(cmdList[1] == "debug")                  BakaOutput(QString::number(debug));
            else                                            HandleInvalidParameter(cmdList[1]);
        }
        else if(cmdList[0] == "auto_fit")                   FitWindow(cmdList[1].toInt(), true);
        else if(cmdList[0] == "open_recent")                OpenRecent(cmdList[1].toInt());
        else                                                HandleInvalidCommand(cmdList[0]+" "+cmdList[1]);
    }
    else if(cmdList.length() == 3)
    {
        if(cmdList[0] == "set")
        {
            if(cmdList[1] == "fullscreen")                  FullScreen((bool)cmdList[2].toInt());
            else if(cmdList[1] == "playlist")               ShowPlaylist((bool)cmdList[2].toInt());
            else if(cmdList[1] == "dim_lights")             DimLights((bool)cmdList[2].toInt());
            else if(cmdList[1] == "debug")                  setDebug((bool)cmdList[2].toInt());
            else                                            HandleInvalidParameter(cmdList[1]);
        }
        else                                                HandleInvalidCommand(cmdList[0]+" "+cmdList[1]+" "+cmdList[2]);
    }
    else
        HandleInvalidCommand(cmdList.join(' '));
}

void MainWindow::HandleInvalidCommand(QString command)
{
    BakaOutput(tr("invalid command '%0'\n").arg(command));
}

void MainWindow::HandleInvalidParameter(QString parameter)
{
    BakaOutput(tr("invalid parameter '%0'\n").arg(parameter));
}

void MainWindow::HandlePlayPause()
{
    if(mpv->getPlayState() > 0)
        mpv->PlayPause(ui->playlistWidget->CurrentItem());
}

void MainWindow::HandlePlayNext()
{
    mpv->PlayFile(ui->playlistWidget->NextItem());
}

void MainWindow::HandlePlayPrevious()
{
    mpv->PlayFile(ui->playlistWidget->PreviousItem());
}

void MainWindow::HandleRewind()
{
    mpv->Rewind();
}

void MainWindow::HandleStop()
{
    mpv->Stop();
}

void MainWindow::HandleMove()
{
    if(!isFullScreen() && !moveTimer)
    {
        moveTimer = new QElapsedTimer();
        moveTimer->start();
        origPos = pos();
    }
}

void MainWindow::HandleBoss()
{
    mpv->Pause();
    if(isFullScreen())
        FullScreen(false);
    setWindowState(windowState() | Qt::WindowMinimized);
}

void MainWindow::HandleOpenFileDialog()
{
    mpv->LoadFile(QFileDialog::getOpenFileName(this,
                   tr("Open File"),mpv->getPath(),
                   tr("Media Files (%0);;").arg(Mpv::media_filetypes.join(" "))+
                   tr("Video Files (%0);;").arg(Mpv::video_filetypes.join(" "))+
                   tr("Audio Files (%0)").arg(Mpv::audio_filetypes.join(" ")),
                   0, QFileDialog::DontUseSheet));
}

void MainWindow::HandleOpenURLDialog()
{
    mpv->LoadFile(LocationDialog::getUrl(mpv->getPath()+mpv->getFile(), this));
}

void MainWindow::HandleOpenClipboard()
{
    mpv->LoadFile(QApplication::clipboard()->text());
}

void MainWindow::HandleJumpDialog()
{
    int time = JumpDialog::getTime(mpv->getFileInfo().length,this);
    if(time >= 0)
        mpv->Seek(time);
}

void MainWindow::HandleShowInFolder()
{
    QDesktopServices::openUrl("file:///"+QDir::toNativeSeparators(mpv->getPath()));
}

void MainWindow::HandleNewPlayer()
{
    QProcess *p = new QProcess(0);
    p->startDetached(QApplication::applicationFilePath());
    delete p;
}

void MainWindow::HandleMediaInfoDialog()
{
    InfoDialog::info(mpv->getPath()+mpv->getFile(), mpv->getFileInfo(), this);
}

void MainWindow::HandlePreferencesDialog()
{
    SaveSettings();
    PreferencesDialog::showPreferences(settings, this);
    LoadSettings();
}

void MainWindow::HandleUpdateDialog()
{
    if(UpdateDialog::update(update, this) == QDialog::Accepted)
    {
        // todo: close and finish update (overwrite self and restart)
    }
}

void MainWindow::HandleOnlineHelp()
{
    QDesktopServices::openUrl(QUrl(tr("http://bakamplayer.u8sand.net/help.php")));
}

void MainWindow::HandleAboutDialog()
{
    AboutDialog::about(BAKA_MPLAYER_VERSION, this);
}

void MainWindow::HandleAboutQtDialog()
{
    qApp->aboutQt();
}

void MainWindow::HandleExit()
{
    close();
}
