/*
LD Worship & Word Evaluation CLI (ldww-eval)
Measures Recall, Latency, and False Positives on audio datasets with ground-truth CSV.
*/

#include "bible.hpp"
#include "detection-core.hpp"
#include "lyrics.hpp"

#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QString>
#include <QTextStream>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <thread>
#include <vector>

struct GroundTruthEvent {
	double timeEndSec = 0.0;
	QString type;     // "scripture" or "lyrics"
	QString expected; // e.g. "Romans 8:28" or "Amazing Grace|Verse 1"
	bool recalled = false;
	double latencySec = 0.0;
};

struct ObservedSuggestion {
	double streamTimeSec = 0.0;
	Channel channel = Channel::Scripture;
	QString reference;
	QString text;
	int confidence = 0;
	bool matchedGroundTruth = false;
};

// Simple WAV audio reader (16-bit int or 32-bit float, mono/stereo, resampled to 16kHz)
static bool readWavFile(const QString &filePath, std::vector<float> &out16kMono, double &durationSec)
{
	QFile f(filePath);
	if (!f.open(QIODevice::ReadOnly)) {
		std::cerr << "Error: Could not open WAV file: " << filePath.toStdString() << "\n";
		return false;
	}

	QByteArray data = f.readAll();
	f.close();

	if (data.size() < 44) {
		std::cerr << "Error: File too small for WAV header\n";
		return false;
	}

	const char *bytes = data.constData();
	if (memcmp(bytes, "RIFF", 4) != 0 || memcmp(bytes + 8, "WAVE", 4) != 0) {
		std::cerr << "Error: Not a valid RIFF/WAVE file\n";
		return false;
	}

	uint16_t audioFormat = 0;
	uint16_t numChannels = 0;
	uint32_t sampleRate = 0;
	uint16_t bitsPerSample = 0;
	const char *audioData = nullptr;
	size_t audioDataSize = 0;

	size_t pos = 12;
	while (pos + 8 <= static_cast<size_t>(data.size())) {
		const char *chunkId = bytes + pos;
		uint32_t chunkSize = *reinterpret_cast<const uint32_t *>(bytes + pos + 4);
		pos += 8;

		if (memcmp(chunkId, "fmt ", 4) == 0 && chunkSize >= 16) {
			audioFormat = *reinterpret_cast<const uint16_t *>(bytes + pos);
			numChannels = *reinterpret_cast<const uint16_t *>(bytes + pos + 2);
			sampleRate = *reinterpret_cast<const uint32_t *>(bytes + pos + 4);
			bitsPerSample = *reinterpret_cast<const uint16_t *>(bytes + pos + 14);
		} else if (memcmp(chunkId, "data", 4) == 0) {
			audioData = bytes + pos;
			audioDataSize = chunkSize;
			break;
		}
		pos += chunkSize;
	}

	if (!audioData || audioDataSize == 0 || sampleRate == 0 || numChannels == 0) {
		std::cerr << "Error: Corrupted WAV or missing 'data' chunk\n";
		return false;
	}

	// Read input samples to float mono
	std::vector<float> inFloatMono;
	size_t numSamples = 0;

	if (audioFormat == 1 && bitsPerSample == 16) {
		const int16_t *pcm = reinterpret_cast<const int16_t *>(audioData);
		size_t totalWords = audioDataSize / sizeof(int16_t);
		numSamples = totalWords / numChannels;
		inFloatMono.reserve(numSamples);

		for (size_t i = 0; i < numSamples; ++i) {
			float sum = 0.0f;
			for (uint16_t ch = 0; ch < numChannels; ++ch) {
				sum += pcm[i * numChannels + ch] / 32768.0f;
			}
			inFloatMono.push_back(sum / numChannels);
		}
	} else if (audioFormat == 3 && bitsPerSample == 32) {
		const float *pcm = reinterpret_cast<const float *>(audioData);
		numSamples = audioDataSize / (sizeof(float) * numChannels);
		inFloatMono.reserve(numSamples);

		for (size_t i = 0; i < numSamples; ++i) {
			float sum = 0.0f;
			for (uint16_t ch = 0; ch < numChannels; ++ch) {
				sum += pcm[i * numChannels + ch];
			}
			inFloatMono.push_back(sum / numChannels);
		}
	} else {
		std::cerr << "Error: Unsupported WAV format (format=" << audioFormat << ", bits=" << bitsPerSample
			  << ")\n";
		return false;
	}

	durationSec = static_cast<double>(numSamples) / static_cast<double>(sampleRate);

	// Resample to 16 kHz if necessary
	if (sampleRate == 16000) {
		out16kMono = std::move(inFloatMono);
	} else {
		double ratio = static_cast<double>(sampleRate) / 16000.0;
		double srcPos = 0.0;
		while (srcPos < static_cast<double>(inFloatMono.size()) - 1.0) {
			size_t idx0 = static_cast<size_t>(srcPos);
			size_t idx1 = idx0 + 1;
			float frac = static_cast<float>(srcPos - idx0);
			float s = (1.0f - frac) * inFloatMono[idx0] + frac * inFloatMono[idx1];
			out16kMono.push_back(s);
			srcPos += ratio;
		}
	}

	return true;
}

static bool readGroundTruth(const QString &csvPath, std::vector<GroundTruthEvent> &outEvents)
{
	QFile f(csvPath);
	if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
		std::cerr << "Error: Could not open ground-truth CSV: " << csvPath.toStdString() << "\n";
		return false;
	}

	QTextStream in(&f);
	bool header = true;
	while (!in.atEnd()) {
		QString line = in.readLine().trimmed();
		if (line.isEmpty() || line.startsWith(QLatin1Char('#')))
			continue;

		if (header && line.contains(QStringLiteral("time_end_seconds"))) {
			header = false;
			continue;
		}

		QStringList parts = line.split(QLatin1Char(','));
		if (parts.size() >= 3) {
			GroundTruthEvent ev;
			ev.timeEndSec = parts[0].trimmed().toDouble();
			ev.type = parts[1].trimmed().toLower();
			ev.expected = parts[2].trimmed();
			outEvents.push_back(ev);
		}
	}
	f.close();
	return !outEvents.empty();
}

static bool matchScripture(const QString &observed, const QString &expected)
{
	QString o = observed.toLower().remove(QRegularExpression(QStringLiteral("[\\s:,]+")));
	QString e = expected.toLower().remove(QRegularExpression(QStringLiteral("[\\s:,]+")));
	if (o == e)
		return true;

	// Canonical variations e.g. "psalm 121" vs "psalms 121"
	if (o.startsWith(QStringLiteral("psalm")) && e.startsWith(QStringLiteral("psalm"))) {
		QString oNum = o;
		oNum.remove(QStringLiteral("psalms")).remove(QStringLiteral("psalm"));
		QString eNum = e;
		eNum.remove(QStringLiteral("psalms")).remove(QStringLiteral("psalm"));
		if (!oNum.isEmpty() && oNum == eNum)
			return true;
	}
	return false;
}

static bool matchLyrics(const QString &observedRef, const QString &observedTitle, const QString &expected)
{
	// expected is "Song Title|Section" e.g. "Amazing Grace|Verse 1"
	QStringList parts = expected.split(QLatin1Char('|'));
	QString expTitle = parts[0].trimmed().toLower();
	QString expSection = parts.size() > 1 ? parts[1].trimmed().toLower() : QString();

	QString obs = observedRef.toLower();
	QString title = observedTitle.toLower();

	bool titleMatched = obs.contains(expTitle) || title.contains(expTitle);
	if (!titleMatched)
		return false;

	if (expSection.isEmpty())
		return true;

	return obs.contains(expSection) || obs.contains(expSection.replace(QStringLiteral("verse "), QStringLiteral("v")));
}

int main(int argc, char *argv[])
{
	QCoreApplication app(argc, argv);
	QCoreApplication::setApplicationName(QStringLiteral("ldww-eval"));
	QCoreApplication::setApplicationVersion(QStringLiteral("1.2.0"));

	QCommandLineParser parser;
	parser.setApplicationDescription(QStringLiteral("LD Worship & Word Speech Evaluation Harness"));
	parser.addHelpOption();
	parser.addVersionOption();

	QCommandLineOption wavOpt(QStringLiteral("wav"), QStringLiteral("Input audio WAV file"), QStringLiteral("path"));
	QCommandLineOption truthOpt(QStringLiteral("truth"), QStringLiteral("Ground-truth CSV file"),
				    QStringLiteral("path"));
	QCommandLineOption modelOpt(QStringLiteral("model"), QStringLiteral("Whisper GGML model file"),
				    QStringLiteral("path"));
	QCommandLineOption songsOpt(QStringLiteral("songs-dir"), QStringLiteral("Songs sample directory"),
				    QStringLiteral("path"));
	QCommandLineOption speedOpt(QStringLiteral("speed"), QStringLiteral("Playback speed (0=unthrottled, 1.0=realtime)"),
				    QStringLiteral("multiplier"), QStringLiteral("0"));
	QCommandLineOption verboseOpt(QStringLiteral("verbose"), QStringLiteral("Enable debug logging"));

	parser.addOption(wavOpt);
	parser.addOption(truthOpt);
	parser.addOption(modelOpt);
	parser.addOption(songsOpt);
	parser.addOption(speedOpt);
	parser.addOption(verboseOpt);
	parser.process(app);

	QString wavPath = parser.value(wavOpt);
	QString truthPath = parser.value(truthOpt);
	QString modelPath = parser.value(modelOpt);
	QString songsDir = parser.value(songsOpt);
	double speed = parser.value(speedOpt).toDouble();
	bool verbose = parser.isSet(verboseOpt);

	if (wavPath.isEmpty() || truthPath.isEmpty()) {
		parser.showHelp(1);
	}

	// 1. Read Audio
	std::vector<float> pcm16k;
	double totalDurationSec = 0.0;
	if (!readWavFile(wavPath, pcm16k, totalDurationSec)) {
		return 1;
	}

	// 2. Read Ground Truth
	std::vector<GroundTruthEvent> truthEvents;
	if (!readGroundTruth(truthPath, truthEvents)) {
		std::cerr << "Error: No ground truth events found in " << truthPath.toStdString() << "\n";
		return 1;
	}

	// 3. Locate Model
	if (modelPath.isEmpty()) {
		QStringList candidates = {
			QStringLiteral("data/models/ggml-base.en.bin"),
			QStringLiteral("data/models/ggml-tiny.en.bin"),
			QStringLiteral("../data/models/ggml-base.en.bin"),
			QStringLiteral("../data/models/ggml-tiny.en.bin"),
			QDir::homePath() + QStringLiteral("/Library/Application Support/obs-studio/plugin_config/ld-scripture/models/ggml-base.en.bin"),
			QDir::homePath() + QStringLiteral("/Library/Application Support/obs-studio/plugin_config/ld-scripture/models/ggml-tiny.en.bin")
		};
		for (const auto &c : candidates) {
			if (QFile::exists(c)) {
				modelPath = c;
				break;
			}
		}
	}

	if (modelPath.isEmpty() || !QFile::exists(modelPath)) {
		std::cerr << "Error: Model path not found. Please specify with --model <path>\n";
		return 1;
	}

	// 4. Load Songs
	if (songsDir.isEmpty()) {
		QStringList candidates = {
			QStringLiteral("data/songs-samples"),
			QStringLiteral("../data/songs-samples"),
			QDir::homePath() + QStringLiteral("/Library/Application Support/obs-studio/plugin_config/ld-scripture/songs")
		};
		for (const auto &c : candidates) {
			if (QDir(c).exists()) {
				songsDir = c;
				break;
			}
		}
	}
	if (!songsDir.isEmpty()) {
		SongLibrary::instance().load(songsDir, QString());
	}

	// 5. Initialize Detection Core
	DetectionCore core;
	if (verbose) {
		core.setDebugLogging(true);
	}

	std::cout << "Loading Whisper model: " << modelPath.toStdString() << "...\n";
	if (!core.loadModel(modelPath)) {
		std::cerr << "Error: Failed to load model\n";
		return 1;
	}

	std::vector<ObservedSuggestion> observations;
	QObject::connect(&core, &DetectionCore::suggestionDetected, [&](const SpeechSuggestion &sugg) {
		ObservedSuggestion obs;
		obs.streamTimeSec = sugg.timestampSeconds;
		obs.channel = sugg.channel;
		obs.reference = sugg.reference;
		obs.text = sugg.text;
		obs.confidence = sugg.confidence;
		observations.push_back(obs);

		if (verbose) {
			std::cout << "  [OBSERVED @" << obs.streamTimeSec << "s] "
				  << (sugg.channel == Channel::Scripture ? "[WORD] " : "[WORSHIP] ")
				  << sugg.reference.toStdString() << " (" << sugg.confidence << "%)\n";
		}
	});

	std::cout << "Streaming " << totalDurationSec << "s audio through detection pipeline...\n";
	auto benchStart = std::chrono::steady_clock::now();

	// Push audio in 0.75s hops (Phase 1)
	constexpr size_t kHopSamples = 12000; // 0.75 seconds @ 16kHz
	size_t totalSamples = pcm16k.size();
	size_t offset = 0;

	while (offset < totalSamples) {
		size_t count = std::min(kHopSamples, totalSamples - offset);
		double streamTime = static_cast<double>(offset + count) / 16000.0;
		core.pushPcmSamples(pcm16k.data() + offset, count, streamTime);
		offset += count;

		if (speed > 0.0) {
			double stepSec = static_cast<double>(count) / 16000.0 / speed;
			std::this_thread::sleep_for(std::chrono::duration<double>(stepSec));
		}
	}

	// Wait for worker to finish processing final window
	std::this_thread::sleep_for(std::chrono::milliseconds(1500));

	auto benchEnd = std::chrono::steady_clock::now();
	double elapsedWallSec = std::chrono::duration<double>(benchEnd - benchStart).count();

	// 6. Match Observations Against Ground Truth
	int scripTotal = 0, scripRecalled = 0, scripFalse = 0;
	int lyricsTotal = 0, lyricsRecalled = 0, lyricsFalse = 0;
	std::vector<double> scripLatencies;
	std::vector<double> lyricsLatencies;

	for (auto &ev : truthEvents) {
		if (ev.type == QStringLiteral("scripture")) {
			scripTotal++;
			for (auto &obs : observations) {
				if (obs.channel == Channel::Scripture && !obs.matchedGroundTruth) {
					if (matchScripture(obs.reference, ev.expected)) {
						double delay = obs.streamTimeSec - ev.timeEndSec;
						// Count as hit if suggested within 3.0s after speech finished
						if (delay >= -1.0 && delay <= 3.0) {
							ev.recalled = true;
							ev.latencySec = std::max(0.0, delay);
							obs.matchedGroundTruth = true;
							scripLatencies.push_back(ev.latencySec);
							break;
						}
					}
				}
			}
			if (ev.recalled)
				scripRecalled++;
		} else if (ev.type == QStringLiteral("lyrics")) {
			lyricsTotal++;
			for (auto &obs : observations) {
				if (obs.channel == Channel::Lyrics && !obs.matchedGroundTruth) {
					if (matchLyrics(obs.reference, obs.text, ev.expected)) {
						double delay = obs.streamTimeSec - ev.timeEndSec;
						if (delay >= -1.0 && delay <= 3.0) {
							ev.recalled = true;
							ev.latencySec = std::max(0.0, delay);
							obs.matchedGroundTruth = true;
							lyricsLatencies.push_back(ev.latencySec);
							break;
						}
					}
				}
			}
			if (ev.recalled)
				lyricsRecalled++;
		}
	}

	for (const auto &obs : observations) {
		if (!obs.matchedGroundTruth) {
			if (obs.channel == Channel::Scripture)
				scripFalse++;
			else
				lyricsFalse++;
		}
	}

	auto computeMedian = [](std::vector<double> v) -> double {
		if (v.empty())
			return 0.0;
		std::sort(v.begin(), v.end());
		return v[v.size() / 2];
	};

	auto computeP90 = [](std::vector<double> v) -> double {
		if (v.empty())
			return 0.0;
		std::sort(v.begin(), v.end());
		size_t idx = static_cast<size_t>(std::ceil(0.90 * v.size())) - 1;
		return v[std::min(idx, v.size() - 1)];
	};

	double scripRecallPct = scripTotal > 0 ? (100.0 * scripRecalled / scripTotal) : 0.0;
	double lyricsRecallPct = lyricsTotal > 0 ? (100.0 * lyricsRecalled / lyricsTotal) : 0.0;
	double minutes = totalDurationSec / 60.0;
	double scripFalse10m = minutes > 0.0 ? (scripFalse / minutes * 10.0) : 0.0;
	double lyricsFalse10m = minutes > 0.0 ? (lyricsFalse / minutes * 10.0) : 0.0;

	double scripMedianLat = computeMedian(scripLatencies);
	double scripP90Lat = computeP90(scripLatencies);
	double lyricsMedianLat = computeMedian(lyricsLatencies);
	double lyricsP90Lat = computeP90(lyricsLatencies);

	std::cout << "\n";
	std::cout << "================================================================================\n";
	std::cout << "                  LD WORSHIP & WORD EVALUATION BENCHMARK\n";
	std::cout << "================================================================================\n";
	std::cout << "Audio File:       " << wavPath.toStdString() << " (" << totalDurationSec << " s)\n";
	std::cout << "Ground Truth:     " << truthPath.toStdString() << " (" << truthEvents.size() << " events)\n";
	std::cout << "Model:            " << QFileInfo(modelPath).fileName().toStdString() << "\n";
	std::cout << "Benchmark Time:   " << elapsedWallSec << " s (Real-time speed: "
		  << (elapsedWallSec > 0 ? (totalDurationSec / elapsedWallSec) : 0.0) << "x)\n";
	std::cout << "--------------------------------------------------------------------------------\n";
	std::cout << "--- SCRIPTURE RESULTS ---\n";
	std::cout << "  Expected Events:    " << scripTotal << "\n";
	std::cout << "  Recalled (<= 3.0s): " << scripRecalled << " (" << scripRecallPct << "%)\n";
	std::cout << "  Missed:             " << (scripTotal - scripRecalled) << "\n";
	std::cout << "  Latency (Median):   " << scripMedianLat << " s\n";
	std::cout << "  Latency (90th Pct): " << scripP90Lat << " s\n";
	std::cout << "  False Suggestions:  " << scripFalse << " (" << scripFalse10m << " per 10 min)\n";
	std::cout << "\n";
	std::cout << "--- LYRICS RESULTS ---\n";
	std::cout << "  Expected Events:    " << lyricsTotal << "\n";
	std::cout << "  Recalled (<= 3.0s): " << lyricsRecalled << " (" << lyricsRecallPct << "%)\n";
	std::cout << "  Missed:             " << (lyricsTotal - lyricsRecalled) << "\n";
	std::cout << "  Latency (Median):   " << lyricsMedianLat << " s\n";
	std::cout << "  Latency (90th Pct): " << lyricsP90Lat << " s\n";
	std::cout << "  False Suggestions:  " << lyricsFalse << " (" << lyricsFalse10m << " per 10 min)\n";
	std::cout << "--------------------------------------------------------------------------------\n";
	std::cout << "--- OVERALL SUMMARY ---\n";
	int totalExpected = scripTotal + lyricsTotal;
	int totalRecalled = scripRecalled + lyricsRecalled;
	double totalRecallPct = totalExpected > 0 ? (100.0 * totalRecalled / totalExpected) : 0.0;
	std::cout << "  Overall Recall:     " << totalRecallPct << "% (" << totalRecalled << "/" << totalExpected << ")\n";
	std::cout << "  Overall False/10m:  " << (scripFalse10m + lyricsFalse10m) << " per 10 min\n";
	std::cout << "================================================================================\n\n";

	return 0;
}
