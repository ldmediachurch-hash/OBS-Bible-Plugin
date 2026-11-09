/*
LD Worship & Word for OBS - Auto Updater
Copyright (C) 2026 LD Media Church

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#pragma once

#include <QFile>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QObject>
#include <QProcess>
#include <QString>
#include <QUrl>

struct UpdateInfo {
	QString pluginId;
	QString name;
	QString latestVersion;
	QString releaseDate;
	QString releaseNotes;
	QString downloadUrl;
	QString purchaseUrl;
	QString supabaseUrl;
	QString supabaseAnonKey;
	bool hasUpdate = false;
	bool requiresLicense = true;
};

class PluginUpdater : public QObject {
	Q_OBJECT

public:
	static PluginUpdater &instance();
	explicit PluginUpdater(QObject *parent = nullptr);
	~PluginUpdater() override;

	static bool isVersionNewer(const QString &latestVersion, const QString &currentVersion);
	static QString currentVersion();

	void checkForUpdates(bool silentIfUpToDate = true);
	void verifyLicense(const QString &email);
	void startDownload(const QString &targetFilename = QString());
	void cancelDownload();

	bool launchInstallerAndQuitOBS(const QString &filePath);

	const UpdateInfo &cachedInfo() const { return m_cachedInfo; }
	bool isDownloading() const { return m_downloadReply != nullptr || m_curlDownloading; }

	bool parseUpdateJson(const QByteArray &data, bool isSilent);
	bool parseLicenseJson(const QByteArray &data, const QString &email);

signals:
	void updateCheckFinished(bool updateAvailable, const UpdateInfo &info, bool isSilent);
	void updateCheckFailed(const QString &errorMessage, bool isSilent);

	void licenseVerificationResult(bool isValid, const QString &message, const QString &email);

	void downloadProgress(qint64 bytesReceived, qint64 bytesTotal);
	void downloadFinished(const QString &filePath);
	void downloadFailed(const QString &errorMessage);

private slots:
	void onCheckFinished();
	void onLicenseFinished();
	void onDownloadReadyRead();
	void onDownloadFinished();

private:
	QNetworkAccessManager m_nam;
	UpdateInfo m_cachedInfo;
	bool m_checkIsSilent = true;

	QNetworkReply *m_checkReply = nullptr;
	QNetworkReply *m_licenseReply = nullptr;
	QString m_verifyingEmail;

	QNetworkReply *m_downloadReply = nullptr;
	QFile *m_downloadFile = nullptr;
	QString m_downloadPath;
	bool m_curlDownloading = false;
	QProcess *m_curlProc = nullptr;
};

