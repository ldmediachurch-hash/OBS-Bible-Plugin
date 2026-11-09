/*
LD Worship & Word for OBS - Auto Updater
Copyright (C) 2026 LD Media Church

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#include "updater.hpp"

#include <obs-module.h>
#include <obs-frontend-api.h>
#include "plugin-support.h"

#include <cmath>
#include <cstring>
#include <thread>

#include <QDesktopServices>
#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMainWindow>
#include <QProcess>
#include <QRegularExpression>
#include <QSslSocket>
#include <QStandardPaths>
#include <QSysInfo>
#include <QUrlQuery>

static bool runCurlGet(const QString &url, const QStringList &headers, QByteArray &outData)
{
	QProcess proc;
	QStringList args;
	args << QStringLiteral("-s") << QStringLiteral("-L") << QStringLiteral("-f")
	     << QStringLiteral("--max-time") << QStringLiteral("12");
	for (const QString &h : headers) {
		args << QStringLiteral("-H") << h;
	}
	args << url;
	proc.start(QStringLiteral("curl"), args);
	if (!proc.waitForFinished(14000)) {
		proc.kill();
		return false;
	}
	if (proc.exitCode() != 0) {
		return false;
	}
	outData = proc.readAllStandardOutput();
	return true;
}

PluginUpdater &PluginUpdater::instance()
{
	static PluginUpdater s_updater;
	return s_updater;
}

PluginUpdater::PluginUpdater(QObject *parent)
	: QObject(parent)
{
}

PluginUpdater::~PluginUpdater()
{
	cancelDownload();
	if (m_checkReply) {
		m_checkReply->abort();
		m_checkReply->deleteLater();
	}
	if (m_licenseReply) {
		m_licenseReply->abort();
		m_licenseReply->deleteLater();
	}
}

QString PluginUpdater::currentVersion()
{
	if (PLUGIN_VERSION && strlen(PLUGIN_VERSION) > 0)
		return QString::fromUtf8(PLUGIN_VERSION);
	return QStringLiteral("1.3.1");
}

bool PluginUpdater::isVersionNewer(const QString &latestVersion, const QString &currentVersion)
{
	auto parseVer = [](QString v) -> QVector<int> {
		if (v.startsWith(QLatin1Char('v'), Qt::CaseInsensitive))
			v = v.mid(1);
		int hyphen = v.indexOf(QLatin1Char('-'));
		if (hyphen != -1)
			v = v.left(hyphen);
		const QStringList parts = v.split(QLatin1Char('.'));
		QVector<int> nums;
		for (const QString &p : parts) {
			nums.append(p.toInt());
		}
		while (nums.size() < 3)
			nums.append(0);
		return nums;
	};

	const QVector<int> l = parseVer(latestVersion);
	const QVector<int> c = parseVer(currentVersion);

	for (int i = 0; i < 3; ++i) {
		if (l[i] > c[i])
			return true;
		if (l[i] < c[i])
			return false;
	}
	return false;
}

bool PluginUpdater::parseUpdateJson(const QByteArray &data, bool isSilent)
{
	QJsonParseError err;
	QJsonDocument doc = QJsonDocument::fromJson(data, &err);
	if (err.error != QJsonParseError::NoError || !doc.isObject()) {
		emit updateCheckFailed(QStringLiteral("Invalid update metadata format"), isSilent);
		return false;
	}

	QJsonObject obj = doc.object();
	m_cachedInfo.pluginId = obj.value(QStringLiteral("plugin_id")).toString();
	m_cachedInfo.name = obj.value(QStringLiteral("name")).toString();
	m_cachedInfo.latestVersion = obj.value(QStringLiteral("latest_version")).toString();
	m_cachedInfo.releaseDate = obj.value(QStringLiteral("release_date")).toString();
	m_cachedInfo.releaseNotes = obj.value(QStringLiteral("release_notes")).toString();
	m_cachedInfo.purchaseUrl = obj.value(QStringLiteral("purchase_url")).toString();
	m_cachedInfo.supabaseUrl = obj.value(QStringLiteral("supabase_url")).toString();
	m_cachedInfo.supabaseAnonKey = obj.value(QStringLiteral("supabase_anon_key")).toString();

#if defined(Q_OS_MACOS)
	m_cachedInfo.downloadUrl = obj.value(QStringLiteral("download_mac")).toString();
#elif defined(Q_OS_WIN)
	m_cachedInfo.downloadUrl = obj.value(QStringLiteral("download_win")).toString();
#else
	m_cachedInfo.downloadUrl = obj.value(QStringLiteral("download_linux")).toString();
#endif

	m_cachedInfo.hasUpdate = isVersionNewer(m_cachedInfo.latestVersion, currentVersion());

	emit updateCheckFinished(m_cachedInfo.hasUpdate, m_cachedInfo, isSilent);
	return true;
}

void PluginUpdater::checkForUpdates(bool silentIfUpToDate)
{
	if (m_checkReply) {
		m_checkReply->abort();
		m_checkReply->deleteLater();
		m_checkReply = nullptr;
	}

	m_checkIsSilent = silentIfUpToDate;

	const QString urlStr = QStringLiteral("https://www.lastdaymedia.com/plugin-update.json");

	if (!QSslSocket::supportsSsl()) {
		std::thread([this, silentIfUpToDate, urlStr]() {
			QByteArray data;
			QStringList headers;
			headers << QStringLiteral("User-Agent: LD-Worship-and-Word/%1").arg(currentVersion());
			if (runCurlGet(urlStr, headers, data)) {
				QMetaObject::invokeMethod(this, [this, data, silentIfUpToDate]() {
					parseUpdateJson(data, silentIfUpToDate);
				}, Qt::QueuedConnection);
			} else {
				QMetaObject::invokeMethod(this, [this, silentIfUpToDate]() {
					emit updateCheckFailed(QStringLiteral("Failed to fetch update metadata"), silentIfUpToDate);
				}, Qt::QueuedConnection);
			}
		}).detach();
		return;
	}

	QUrl url(urlStr);
	QNetworkRequest req(url);
	req.setHeader(QNetworkRequest::UserAgentHeader,
		      QStringLiteral("LD-Worship-and-Word/%1 (%2)").arg(currentVersion(), QSysInfo::prettyProductName()));
	req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);

	m_checkReply = m_nam.get(req);
	connect(m_checkReply, &QNetworkReply::finished, this, &PluginUpdater::onCheckFinished);
}

void PluginUpdater::onCheckFinished()
{
	if (!m_checkReply)
		return;

	QNetworkReply *reply = m_checkReply;
	m_checkReply = nullptr;
	reply->deleteLater();

	if (reply->error() != QNetworkReply::NoError) {
		const QString urlStr = QStringLiteral("https://www.lastdaymedia.com/plugin-update.json");
		bool silent = m_checkIsSilent;
		std::thread([this, silent, urlStr]() {
			QByteArray data;
			QStringList headers;
			headers << QStringLiteral("User-Agent: LD-Worship-and-Word/%1").arg(currentVersion());
			if (runCurlGet(urlStr, headers, data)) {
				QMetaObject::invokeMethod(this, [this, data, silent]() {
					parseUpdateJson(data, silent);
				}, Qt::QueuedConnection);
			} else {
				QMetaObject::invokeMethod(this, [this, silent]() {
					emit updateCheckFailed(QStringLiteral("Failed to reach update server"), silent);
				}, Qt::QueuedConnection);
			}
		}).detach();
		return;
	}

	parseUpdateJson(reply->readAll(), m_checkIsSilent);
}

bool PluginUpdater::parseLicenseJson(const QByteArray &data, const QString &email)
{
	QJsonDocument doc = QJsonDocument::fromJson(data);
	if (!doc.isArray()) {
		emit licenseVerificationResult(false, QStringLiteral("Unexpected verification server response."),
					       email);
		return false;
	}

	QJsonArray arr = doc.array();
	bool foundWorshipPurchase = false;
	for (const QJsonValue v : arr) {
		QJsonObject rec = v.toObject();
		QString title = rec.value(QStringLiteral("resource_title")).toString();
		if (title.contains(QStringLiteral("Worship"), Qt::CaseInsensitive) ||
		    title.contains(QStringLiteral("Plugin"), Qt::CaseInsensitive) ||
		    title.contains(QStringLiteral("LD Scripture"), Qt::CaseInsensitive)) {
			foundWorshipPurchase = true;
			break;
		}
	}

	if (foundWorshipPurchase) {
		emit licenseVerificationResult(true, QStringLiteral("Purchase verified successfully!"),
					       email);
	} else {
		emit licenseVerificationResult(
			false, QStringLiteral("No matching purchase found for this email on lastdaymedia.com."),
			email);
	}
	return true;
}

void PluginUpdater::verifyLicense(const QString &email)
{
	if (m_licenseReply) {
		m_licenseReply->abort();
		m_licenseReply->deleteLater();
		m_licenseReply = nullptr;
	}

	m_verifyingEmail = email.trimmed();
	if (m_verifyingEmail.isEmpty() || !m_verifyingEmail.contains(QLatin1Char('@'))) {
		emit licenseVerificationResult(false, QStringLiteral("Please enter a valid email address."),
					       m_verifyingEmail);
		return;
	}

	QString supaUrl = m_cachedInfo.supabaseUrl;
	if (supaUrl.isEmpty()) {
		supaUrl = QStringLiteral("https://eduaomfieovlzzrwgsct.supabase.co");
	}
	QString anonKey = m_cachedInfo.supabaseAnonKey;
	if (anonKey.isEmpty()) {
		anonKey = QStringLiteral(
			"eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJpc3MiOiJzdXBhYmFzZSIsInJlZiI6ImVkdWFvbWZpZW92bHp6cndnc2N0Iiwicm9sZSI6ImFub24iLCJpYXQiOjE3ODI5NDE1NzUsImV4cCI6MjA5ODUxNzU3NX0.o2mr8eMX-67lMVISPOO5Re12tMBRWodPKg_Xc3U4WnY");
	}

	QUrl url(supaUrl + QStringLiteral("/rest/v1/leads"));
	QUrlQuery query;
	query.addQueryItem(QStringLiteral("email"), QStringLiteral("eq.") + m_verifyingEmail.toLower());
	query.addQueryItem(QStringLiteral("select"), QStringLiteral("id,resource_title,created_at"));
	url.setQuery(query);

	QString endpoint = url.toString();
	QStringList headers;
	headers << QStringLiteral("apikey: %1").arg(anonKey);
	headers << QStringLiteral("Authorization: Bearer %1").arg(anonKey);
	headers << QStringLiteral("User-Agent: LD-Worship-and-Word/%1").arg(currentVersion());

	if (!QSslSocket::supportsSsl()) {
		std::thread([this, vEmail = m_verifyingEmail, endpoint, headers]() {
			QByteArray data;
			if (runCurlGet(endpoint, headers, data)) {
				QMetaObject::invokeMethod(this, [this, data, vEmail]() {
					parseLicenseJson(data, vEmail);
				}, Qt::QueuedConnection);
			} else {
				QMetaObject::invokeMethod(this, [this, vEmail]() {
					emit licenseVerificationResult(false, QStringLiteral("Could not reach verification server."), vEmail);
				}, Qt::QueuedConnection);
			}
		}).detach();
		return;
	}

	QNetworkRequest req(url);
	req.setRawHeader("apikey", anonKey.toUtf8());
	req.setRawHeader("Authorization", "Bearer " + anonKey.toUtf8());
	req.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("LD-Worship-and-Word/%1").arg(currentVersion()));
	req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);

	m_licenseReply = m_nam.get(req);
	connect(m_licenseReply, &QNetworkReply::finished, this, &PluginUpdater::onLicenseFinished);
}

void PluginUpdater::onLicenseFinished()
{
	if (!m_licenseReply)
		return;

	QNetworkReply *reply = m_licenseReply;
	m_licenseReply = nullptr;
	reply->deleteLater();

	if (reply->error() != QNetworkReply::NoError) {
		QString supaUrl = m_cachedInfo.supabaseUrl;
		if (supaUrl.isEmpty())
			supaUrl = QStringLiteral("https://eduaomfieovlzzrwgsct.supabase.co");
		QString anonKey = m_cachedInfo.supabaseAnonKey;
		if (anonKey.isEmpty())
			anonKey = QStringLiteral(
				"eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJpc3MiOiJzdXBhYmFzZSIsInJlZiI6ImVkdWFvbWZpZW92bHp6cndnc2N0Iiwicm9sZSI6ImFub24iLCJpYXQiOjE3ODI5NDE1NzUsImV4cCI6MjA5ODUxNzU3NX0.o2mr8eMX-67lMVISPOO5Re12tMBRWodPKg_Xc3U4WnY");

		QUrl url(supaUrl + QStringLiteral("/rest/v1/leads"));
		QUrlQuery query;
		query.addQueryItem(QStringLiteral("email"), QStringLiteral("eq.") + m_verifyingEmail.toLower());
		query.addQueryItem(QStringLiteral("select"), QStringLiteral("id,resource_title,created_at"));
		url.setQuery(query);

		QString endpoint = url.toString();
		QStringList headers;
		headers << QStringLiteral("apikey: %1").arg(anonKey);
		headers << QStringLiteral("Authorization: Bearer %1").arg(anonKey);

		std::thread([this, vEmail = m_verifyingEmail, endpoint, headers]() {
			QByteArray data;
			if (runCurlGet(endpoint, headers, data)) {
				QMetaObject::invokeMethod(this, [this, data, vEmail]() {
					parseLicenseJson(data, vEmail);
				}, Qt::QueuedConnection);
			} else {
				QMetaObject::invokeMethod(this, [this, vEmail]() {
					emit licenseVerificationResult(false, QStringLiteral("Could not reach verification server."), vEmail);
				}, Qt::QueuedConnection);
			}
		}).detach();
		return;
	}

	parseLicenseJson(reply->readAll(), m_verifyingEmail);
}

void PluginUpdater::startDownload(const QString &targetFilename)
{
	cancelDownload();

	if (m_cachedInfo.downloadUrl.isEmpty()) {
		emit downloadFailed(QStringLiteral("No download URL available for this platform."));
		return;
	}

	QString filename = targetFilename;
	if (filename.isEmpty()) {
		QUrl url(m_cachedInfo.downloadUrl);
		filename = url.fileName();
		if (filename.isEmpty()) {
#if defined(Q_OS_MACOS)
			filename = QStringLiteral("LD-Worship-and-Word-%1-macos-universal.pkg")
					   .arg(m_cachedInfo.latestVersion);
#elif defined(Q_OS_WIN)
			filename =
				QStringLiteral("LD-Worship-and-Word-%1-windows-x64.zip").arg(m_cachedInfo.latestVersion);
#else
			filename = QStringLiteral("LD-Worship-and-Word-%1-linux.deb").arg(m_cachedInfo.latestVersion);
#endif
		}
	}

	QString tempDir = QStandardPaths::writableLocation(QStandardPaths::TempLocation);
	m_downloadPath = QDir(tempDir).filePath(filename);

	if (!QSslSocket::supportsSsl()) {
		m_curlDownloading = true;
		emit downloadProgress(0, 100);

		m_curlProc = new QProcess(this);
		connect(m_curlProc, &QProcess::readyReadStandardError, [this]() {
			if (!m_curlProc)
				return;
			QByteArray err = m_curlProc->readAllStandardError();
			static QRegularExpression rx(QStringLiteral(R"((\d{1,3}(?:\.\d+)?)%)"));
			QRegularExpressionMatchIterator it = rx.globalMatch(QString::fromUtf8(err));
			int lastPct = -1;
			while (it.hasNext()) {
				auto match = it.next();
				lastPct = static_cast<int>(std::round(match.captured(1).toDouble()));
			}
			if (lastPct >= 0) {
				emit downloadProgress(lastPct, 100);
			}
		});

		connect(m_curlProc, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
			[this](int exitCode, QProcess::ExitStatus) {
				m_curlDownloading = false;
				if (m_curlProc) {
					m_curlProc->deleteLater();
					m_curlProc = nullptr;
				}
				if (exitCode == 0 && QFile::exists(m_downloadPath)) {
					emit downloadProgress(100, 100);
					emit downloadFinished(m_downloadPath);
				} else {
					QFile::remove(m_downloadPath);
					emit downloadFailed(QStringLiteral("Download interrupted or failed."));
				}
			});

		QStringList args;
		args << QStringLiteral("-#") << QStringLiteral("-L") << QStringLiteral("-o") << m_downloadPath
		     << m_cachedInfo.downloadUrl;
		m_curlProc->start(QStringLiteral("curl"), args);
		return;
	}

	m_downloadFile = new QFile(m_downloadPath, this);
	if (!m_downloadFile->open(QIODevice::WriteOnly | QIODevice::Truncate)) {
		delete m_downloadFile;
		m_downloadFile = nullptr;
		emit downloadFailed(
			QStringLiteral("Failed to create temporary download file: %1").arg(m_downloadPath));
		return;
	}

	QNetworkRequest req(QUrl(m_cachedInfo.downloadUrl));
	req.setHeader(QNetworkRequest::UserAgentHeader,
		      QStringLiteral("LD-Worship-and-Word-Updater/%1").arg(currentVersion()));
	req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);

	m_downloadReply = m_nam.get(req);
	connect(m_downloadReply, &QNetworkReply::readyRead, this, &PluginUpdater::onDownloadReadyRead);
	connect(m_downloadReply, &QNetworkReply::downloadProgress, this, &PluginUpdater::downloadProgress);
	connect(m_downloadReply, &QNetworkReply::finished, this, &PluginUpdater::onDownloadFinished);
}

void PluginUpdater::onDownloadReadyRead()
{
	if (m_downloadFile && m_downloadReply) {
		m_downloadFile->write(m_downloadReply->readAll());
	}
}

void PluginUpdater::onDownloadFinished()
{
	if (!m_downloadReply)
		return;

	QNetworkReply *reply = m_downloadReply;
	m_downloadReply = nullptr;
	reply->deleteLater();

	if (m_downloadFile) {
		m_downloadFile->write(reply->readAll());
		m_downloadFile->flush();
		m_downloadFile->close();
		delete m_downloadFile;
		m_downloadFile = nullptr;
	}

	if (reply->error() != QNetworkReply::NoError) {
		QFile::remove(m_downloadPath);
		emit downloadFailed(reply->errorString());
		return;
	}

	emit downloadFinished(m_downloadPath);
}

void PluginUpdater::cancelDownload()
{
	if (m_curlProc) {
		m_curlProc->kill();
		m_curlProc->deleteLater();
		m_curlProc = nullptr;
		m_curlDownloading = false;
	}
	if (m_downloadReply) {
		m_downloadReply->abort();
		m_downloadReply->deleteLater();
		m_downloadReply = nullptr;
	}
	if (m_downloadFile) {
		m_downloadFile->close();
		delete m_downloadFile;
		m_downloadFile = nullptr;
		QFile::remove(m_downloadPath);
	}
}

bool PluginUpdater::launchInstallerAndQuitOBS(const QString &filePath)
{
	if (!QFile::exists(filePath))
		return false;

	bool started = false;
#if defined(Q_OS_MACOS)
	started = QProcess::startDetached(QStringLiteral("/usr/bin/open"), {filePath});
#elif defined(Q_OS_WIN)
	started = QDesktopServices::openUrl(QUrl::fromLocalFile(filePath));
#else
	started = QDesktopServices::openUrl(QUrl::fromLocalFile(filePath));
#endif

	if (started) {
		QMainWindow *mainWin = (QMainWindow *)obs_frontend_get_main_window();
		if (mainWin) {
			mainWin->close();
		}
		return true;
	}
	return false;
}

