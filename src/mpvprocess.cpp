/*  smplayer, GUI front-end for mplayer.
    Copyright (C) 2006-2024 Ricardo Villalba <ricardo@smplayer.info>

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
*/

#include "mpvprocess.h"
#include <QRegExp>
#include <QStringList>
#include <QApplication>
#include <QDebug>

#include "global.h"
#include "preferences.h"
#include "mplayerversion.h"
#include "colorutils.h"

#ifdef USE_IPC
#include <QLocalSocket>
#endif

#ifdef USE_IPC
 #if QT_VERSION >= 0x050000
 #include <QStandardPaths>
 #else
 #include <QDesktopServices>
 #endif

 #define IPC_STATUS
#endif

using namespace Global;

#define CUSTOM_STATUS

#define TOO_CHAPTERS_WORKAROUND

MPVProcess::MPVProcess(QObject * parent)
	: PlayerProcess(parent)
	, notified_mplayer_is_running(false)
	, notified_pause(false)
	, received_end_of_file(false)
	, last_sub_id(-1)
	, mplayer_svn(-1) // Not found yet
	, verbose(false)
#if NOTIFY_SUB_CHANGES
	, subtitle_info_received(false)
	, subtitle_info_changed(false)
	, selected_subtitle(-1)
#endif
#if NOTIFY_AUDIO_CHANGES
	, audio_info_changed(false)
	, selected_audio(-1)
#endif
#if NOTIFY_VIDEO_CHANGES
	, video_info_changed(false)
	, selected_video(-1)
#endif
#if NOTIFY_CHAPTER_CHANGES
	, chapter_info_changed(false)
#endif
	, dvd_current_title(-1)
	, br_current_title(-1)
{
	player_id = PlayerID::MPV;

	connect( this, SIGNAL(lineAvailable(QByteArray)),
			 this, SLOT(parseLine(QByteArray)) );

	connect( this, SIGNAL(finished(int,QProcess::ExitStatus)), 
             this, SLOT(processFinished(int,QProcess::ExitStatus)) );

	connect( this, SIGNAL(error(QProcess::ProcessError)),
             this, SLOT(gotError(QProcess::ProcessError)) );

	/* int svn = MplayerVersion::mplayerVersion("mpv unknown version (C)"); */

	initializeOptionVars();
	initializeRX();

#ifdef USE_IPC
	socket = new QLocalSocket(this);
	connect(socket, SIGNAL(readyRead()), this, SLOT(socketReadyRead()));
	#if QT_VERSION >= 0x050000
	QString temp_dir = QStandardPaths::writableLocation(QStandardPaths::TempLocation);
	#else
	QString temp_dir = QDesktopServices::storageLocation(QDesktopServices::TempLocation);
	#endif
	socket_name = temp_dir + "/smplayer-mpv-" + QString::number(QCoreApplication::applicationPid(), 16);
	qDebug() << "MPVProcess::MPVProcess: socket_name:" << socket_name;
#endif
}

MPVProcess::~MPVProcess() {
#ifdef USE_IPC
	if (QFile::exists(socket_name)) {
		QFile::remove(socket_name);
	}
#endif
}

#ifdef USE_IPC
void MPVProcess::setSocketName(const QString & name) {
	// Remove old socket
	if (!socket_name.isEmpty() && QFile::exists(socket_name)) {
		QFile::remove(socket_name);
	}
	socket_name = name;
	qDebug() << "MPVProcess::setSocketName:" << socket_name;
}
#endif

bool MPVProcess::start() {
	md.reset();
	notified_mplayer_is_running = false;
	notified_pause = false;
	last_sub_id = -1;
	mplayer_svn = -1; // Not found yet
	received_end_of_file = false;

#if NOTIFY_SUB_CHANGES
	subs.clear();
	subtitle_info_received = false;
	subtitle_info_changed = false;
	selected_subtitle = -1;
#endif

#if NOTIFY_AUDIO_CHANGES
	audios.clear();
	audio_info_changed = false;
	selected_audio = -1;
#endif

#if NOTIFY_VIDEO_CHANGES
	videos.clear();
	video_info_changed = false;
	selected_video = -1;
#endif

#if NOTIFY_CHAPTER_CHANGES
	chapters.clear();
	chapter_info_changed = false;
#endif

	dvd_current_title = -1;
	br_current_title = -1;

	MyProcess::start();

	return waitForStarted();
}

void MPVProcess::initializeRX() {
	rx_notification = QRegExp("property-change.*\"name\":\"([a-z-/]+)\".*\"data\":([a-z0-9-.\"]+)");
}

void MPVProcess::parseLine(QByteArray ba) {
	if (socket->state() != QLocalSocket::ConnectedState) {
		sendCommand("{ \"command\": [\"get_property\", \"duration\"], \"request_id\": 100 }");
	}
}

void MPVProcess::requestChapterInfo() {
	sendCommand("print_text \"INFO_CHAPTERS=${=chapters}\"");
}

#if NOTIFY_VIDEO_CHANGES
void MPVProcess::updateVideoTrack(int ID, const QString & name, const QString & lang, bool selected) {
	qDebug("MPVProcess::updateVideoTrack: ID: %d", ID);

	int idx = videos.find(ID);
	if (idx == -1) {
		video_info_changed = true;
		videos.addName(ID, name);
		videos.addLang(ID, lang);
	} else {
		// Track already existed
		if (videos.itemAt(idx).name() != name) {
			video_info_changed = true;
			videos.addName(ID, name);
		}
		if (videos.itemAt(idx).lang() != lang) {
			video_info_changed = true;
			videos.addLang(ID, lang);
		}
	}

	if (selected && selected_video != ID) {
		selected_video = ID;
		video_info_changed = true;
	}
}
#endif

#if NOTIFY_AUDIO_CHANGES
void MPVProcess::updateAudioTrack(int ID, const QString & name, const QString & lang, bool selected) {
	qDebug("MPVProcess::updateAudioTrack: ID: %d", ID);

	int idx = audios.find(ID);
	if (idx == -1) {
		audio_info_changed = true;
		audios.addName(ID, name);
		audios.addLang(ID, lang);
	} else {
		// Track already existed
		if (audios.itemAt(idx).name() != name) {
			audio_info_changed = true;
			audios.addName(ID, name);
		}
		if (audios.itemAt(idx).lang() != lang) {
			audio_info_changed = true;
			audios.addLang(ID, lang);
		}
	}

	if (selected && selected_audio != ID) {
		selected_audio = ID;
		audio_info_changed = true;
	}
}
#endif

#if NOTIFY_SUB_CHANGES
void MPVProcess::updateSubtitleTrack(int ID, const QString & name, const QString & lang, bool selected) {
	qDebug("MPVProcess::updateSubtitleTrack: ID: %d", ID);

	int idx = subs.find(SubData::Sub, ID);
	if (idx == -1) {
		subtitle_info_changed = true;
		subs.add(SubData::Sub, ID);
		subs.changeName(SubData::Sub, ID, name);
		subs.changeLang(SubData::Sub, ID, lang);
	}
	else {
		// Track already existed
		if (subs.itemAt(idx).name() != name) {
			subtitle_info_changed = true;
			subs.changeName(SubData::Sub, ID, name);
		}
		if (subs.itemAt(idx).lang() != lang) {
			subtitle_info_changed = true;
			subs.changeLang(SubData::Sub, ID, lang);
		}
	}

	if (selected && selected_subtitle != ID) {
		selected_subtitle = ID;
		subtitle_info_changed = true;
	}
}
#endif

// Called when the process is finished
void MPVProcess::processFinished(int exitCode, QProcess::ExitStatus exitStatus) {
	qDebug("MPVProcess::processFinished: exitCode: %d, status: %d", exitCode, (int) exitStatus);
	// Send this signal before the endoffile one, otherwise
	// the playlist will start to play next file before all
	// objects are notified that the process has exited.
	emit processExited();
	if (received_end_of_file) emit receivedEndOfFile();
}

void MPVProcess::gotError(QProcess::ProcessError error) {
	qDebug("MPVProcess::gotError: %d", (int) error);
}

#ifdef USE_IPC
void MPVProcess::sendCommand(QString text) {
	qDebug() << "MPVProcess::sendCommand:" << text;

	if (!isRunning()) {
		qWarning("MPVProcess::sendCommand: mpv is not running. Command ignored.");
		return;
	}

	if (socket->state() != QLocalSocket::ConnectedState) {
		qDebug() << "MPVProcess::sendCommand: state:" << socket->state();
		qDebug() << "MPVProcess::sendCommand: error:" << socket->errorString();
		socket->close();
		socket->connectToServer(socket_name, QIODevice::ReadWrite | QIODevice::Text);
		socket->waitForConnected();
		qDebug() << "MPVProcess::sendCommand: state:" << socket->state();
		#ifdef IPC_STATUS
		socket->write("{ \"command\": [\"observe_property\", 1, \"pause\"] }\n"
                      "{ \"command\": [\"observe_property\", 2, \"paused-for-cache\"] }\n"
                      "{ \"command\": [\"observe_property\", 3, \"core-idle\"] }\n"
                      "{ \"command\": [\"observe_property\", 4, \"video-bitrate\"] }\n"
                      "{ \"command\": [\"observe_property\", 5, \"audio-bitrate\"] }\n"
                      "{ \"command\": [\"observe_property\", 6, \"duration\"] }\n"
                      "{ \"command\": [\"observe_property\", 7, \"width\"] }\n"
                      "{ \"command\": [\"observe_property\", 8, \"dheight\"] }\n"
                      "{ \"command\": [\"observe_property\", 9, \"dwidth\"] }\n"
                      "{ \"command\": [\"observe_property\", 10, \"height\"] }\n"
                      "{ \"command\": [\"observe_property\", 11, \"video-params/aspect\"] }\n"
                      "{ \"command\": [\"observe_property\", 12, \"container-fps\"] }\n"
                      "{ \"command\": [\"observe_property\", 13, \"video-format\"] }\n"
                      "{ \"command\": [\"observe_property\", 100, \"time-pos\"] }\n"
                     );
		#endif
	}
	socket->write(text.toUtf8() +"\n");
	socket->flush();
}

void MPVProcess::socketReadyRead() {
	#ifdef IPC_STATUS
	//qDebug("MPVProcess::socketReadyRead");
	static double last_sec = -1;
	static int dwidth = 0;
	static int dheight = 0;

	while (socket->canReadLine()) {
		QString s = socket->readLine();
		if (s.indexOf("time-pos") == -1) {
			qDebug() << "MPVProcess::socketReadyRead:" << s;
		}
		if (rx_notification.indexIn(s) > -1) {
			//qDebug() << "MPVProcess::socketReadyRead:" << rx_notification;
			QString name = rx_notification.cap(1);
			QString data = rx_notification.cap(2);
			if (name != "time-pos") {
				qDebug() << "MPVProcess::socketReadyRead:" << name << data;
			}
			if (name == "pause" && data == "true") emit receivedPause();
			else
			if (name == "paused-for-cache" && data == "true") emit receivedBuffering();
			//else
			//if (name == "core-idle" && data == "true") emit receivedBuffering();
			else
			if (name == "video-bitrate") {
				int video_bitrate = data.toInt();
				md.video_bitrate = video_bitrate;
				emit receivedVideoBitrate(video_bitrate);
			}
			else
			if (name == "audio-bitrate") {
				int audio_bitrate = data.toInt();
				md.audio_bitrate = audio_bitrate;
				emit receivedAudioBitrate(audio_bitrate);
			}
			else
			if (name == "time-pos") {
				double sec = data.toDouble();
				if (!notified_mplayer_is_running) {
					emit receivedStartingTime(sec);
					emit mplayerFullyLoaded();
					emit receivedCurrentFrame(0); // Set the frame counter to 0
					notified_mplayer_is_running = true;
				}
				if (last_sec != sec) {
					last_sec = sec;
					emit receivedCurrentSec(sec);
				}
			}
			else
			if (name == "duration") {
				md.duration = data.toDouble();
			}
			else
			if (name == "width") {
				md.video_width = data.toInt();
			}
			else
			if (name == "height") {
				md.video_height = data.toInt();
			}
			else
			if (name == "dwidth" || name == "dheight") {
				if (name == "dwidth") dwidth = data.toInt(); else dheight = data.toInt();
				if (dwidth != 0 && dheight != 0) emit receivedWindowResolution( dwidth, dheight );
			}
			else
			if (name == "video-params/aspect") {
				md.video_aspect = data.toDouble();
			}
			else
			if (name == "container-fps") {
				md.video_fps = data;
			}
			else
			if (name == "video-format") {
				md.video_format = data.replace("\"", "");
				md.video_codec = md.video_format;
			}
		}
	}
	#endif
}
#endif

#include "mpvoptions.cpp"
#include "moc_mpvprocess.cpp"
