#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>
#include <algorithm>

#include <whisper.h>
#include "scripture/scripture_detect.hpp"
#include "scripture/scripture_bible_structure.hpp"

struct TimedWord {
	std::string text;
	double startTimeSec = 0.0;
	double endTimeSec = 0.0;
};

static std::string cleanWord(const std::string &w)
{
	std::string res;
	for (char c : w) {
		if (std::isalnum(static_cast<unsigned char>(c))) {
			res += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
		}
	}
	return res;
}

static bool isSentenceEnd(const std::string &w)
{
	if (w.empty())
		return false;
	for (auto it = w.rbegin(); it != w.rend(); ++it) {
		char c = *it;
		if (c == '.' || c == '?' || c == '!')
			return true;
		if (std::isalnum(static_cast<unsigned char>(c)))
			break;
	}
	return false;
}

static bool isStockSilence(const std::string &text)
{
	std::string c = cleanWord(text);
	if (c.empty())
		return true;
	if (c == "thankyou" || c == "thankyouverymuch" || c == "thanksforwatching" ||
	    c == "blankaudio" || c == "amen" || c == "you" || c == "subtitlesby" ||
	    c == "thankyouforwatching" || c == "bye" || c == "goodbye") {
		return true;
	}
	std::string trimmed;
	for (char ch : text) {
		if (!std::isspace(static_cast<unsigned char>(ch)))
			trimmed += ch;
	}
	if (!trimmed.empty()) {
		if ((trimmed.front() == '[' && trimmed.back() == ']') ||
		    (trimmed.front() == '(' && trimmed.back() == ')')) {
			return true;
		}
	}
	return false;
}

static bool isMostlyPrompt(const std::string &segText)
{
	std::string cSeg = cleanWord(segText);
	if (cSeg.empty())
		return true;

	static const std::string cStaticPrompt = cleanWord("A sermon. The preacher reads from the Bible.");
	if (cSeg.length() >= 6 && cStaticPrompt.find(cSeg) != std::string::npos) {
		return true;
	}

	const std::vector<std::string> promptWords = {"a", "sermon", "the", "preacher", "reads", "from", "bible"};
	std::stringstream ss(segText);
	std::string token;
	int totalWords = 0;
	int promptWordMatches = 0;
	while (ss >> token) {
		std::string c = cleanWord(token);
		if (c.empty())
			continue;
		totalWords++;
		for (const auto &pw : promptWords) {
			if (c == pw) {
				promptWordMatches++;
				break;
			}
		}
	}
	if (totalWords > 0 && (static_cast<float>(promptWordMatches) / static_cast<float>(totalWords)) >= 0.7f) {
		return true;
	}
	return false;
}

static bool hasRepetitionLoop(const std::vector<TimedWord> &words)
{
	if (words.size() < 6)
		return false;

	std::vector<std::string> clean;
	clean.reserve(words.size());
	for (const auto &w : words) {
		std::string c = cleanWord(w.text);
		if (!c.empty())
			clean.push_back(c);
	}

	int n = static_cast<int>(clean.size());
	if (n < 6)
		return false;

	// Check for consecutive repetitions of 2 to 8 word phrases (3+ times)
	for (int len = 2; len <= 8; ++len) {
		for (int i = 0; i + 3 * len <= n; ++i) {
			bool match = true;
			for (int k = 1; k < 3; ++k) {
				for (int j = 0; j < len; ++j) {
					if (clean[i + j] != clean[i + k * len + j]) {
						match = false;
						break;
					}
				}
				if (!match)
					break;
			}
			if (match)
				return true;
		}
	}
	return false;
}

static std::vector<TimedWord> getUncommittedWords(
	const std::vector<TimedWord> &words,
	const std::string &committedHistory,
	double lastCommittedTimeSec)
{
	if (words.empty())
		return {};
	if (committedHistory.empty() || lastCommittedTimeSec < 0.0)
		return words;

	std::vector<std::string> histWords;
	std::stringstream ss(committedHistory);
	std::string tw;
	while (ss >> tw) {
		std::string c = cleanWord(tw);
		if (!c.empty())
			histWords.push_back(c);
	}

	int matchIdx = -1;
	for (int i = 0; i < static_cast<int>(words.size()); ++i) {
		if (std::abs(words[i].endTimeSec - lastCommittedTimeSec) < 0.8) {
			std::string cw = cleanWord(words[i].text);
			if (!histWords.empty() && cw == histWords.back()) {
				matchIdx = i;
				break;
			}
		}
	}

	if (matchIdx >= 0) {
		if (matchIdx + 1 < static_cast<int>(words.size()))
			return std::vector<TimedWord>(words.begin() + matchIdx + 1, words.end());
		return {};
	}

	std::vector<TimedWord> uncommitted;
	for (const auto &w : words) {
		if (w.endTimeSec > lastCommittedTimeSec - 0.05) {
			uncommitted.push_back(w);
		}
	}
	return uncommitted;
}

static void applyHighPass80Hz(std::vector<float> &audio, float sampleRate = 16000.0f)
{
	if (audio.empty())
		return;

	constexpr float f0 = 80.0f;
	constexpr float q = 0.70710678f;
	const float omega0 = 2.0f * static_cast<float>(M_PI) * (f0 / sampleRate);
	const float sinOmega = std::sin(omega0);
	const float cosOmega = std::cos(omega0);
	const float alpha = sinOmega / (2.0f * q);

	const float b0 = (1.0f + cosOmega) * 0.5f;
	const float b1 = -(1.0f + cosOmega);
	const float b2 = (1.0f + cosOmega) * 0.5f;
	const float a0 = 1.0f + alpha;
	const float a1 = -2.0f * cosOmega;
	const float a2 = 1.0f - alpha;

	const float b0_n = b0 / a0;
	const float b1_n = b1 / a0;
	const float b2_n = b2 / a0;
	const float a1_n = a1 / a0;
	const float a2_n = a2 / a0;

	float x1 = 0.0f, x2 = 0.0f;
	float y1 = 0.0f, y2 = 0.0f;

	for (float &x : audio) {
		float y = b0_n * x + b1_n * x1 + b2_n * x2 - a1_n * y1 - a2_n * y2;
		x2 = x1;
		x1 = x;
		y2 = y1;
		y1 = y;
		x = y;
	}
}

static void applyAgcTo20dBFS(std::vector<float> &audio)
{
	if (audio.empty())
		return;

	constexpr float targetRms = 0.10f; // -20 dBFS RMS
	constexpr size_t subSize = 3200;   // 200 ms @ 16 kHz

	float sumSpeechSq = 0.0f;
	size_t speechSamples = 0;

	for (size_t i = 0; i + subSize <= audio.size(); i += subSize) {
		float subSum = 0.0f;
		for (size_t j = 0; j < subSize; ++j) {
			float s = audio[i + j];
			subSum += s * s;
		}
		float subRms = std::sqrt(subSum / static_cast<float>(subSize));
		if (subRms > 0.005f) {
			sumSpeechSq += subSum;
			speechSamples += subSize;
		}
	}

	if (speechSamples > 0) {
		float speechRms = std::sqrt(sumSpeechSq / static_cast<float>(speechSamples));
		if (speechRms > 0.001f) {
			float gain = targetRms / speechRms;
			gain = std::clamp(gain, 0.2f, 10.0f);
			for (float &s : audio) {
				s = std::clamp(s * gain, -1.0f, 1.0f);
			}
		}
	}
}

static bool loadWav16kMono(const std::string &path, std::vector<float> &outSamples)
{
	std::ifstream f(path, std::ios::binary);
	if (!f.is_open()) {
		std::cerr << "Cannot open WAV file: " << path << "\n";
		return false;
	}

	char header[12];
	if (!f.read(header, 12) || std::memcmp(header, "RIFF", 4) != 0 || std::memcmp(header + 8, "WAVE", 4) != 0) {
		std::cerr << "Not a RIFF/WAVE file\n";
		return false;
	}

	uint16_t audioFormat = 0, numChannels = 0, bitsPerSample = 0;
	uint32_t sampleRate = 0;
	std::vector<char> dataBuffer;

	while (f) {
		char chunkId[4];
		uint32_t chunkSize = 0;
		if (!f.read(chunkId, 4) || !f.read(reinterpret_cast<char *>(&chunkSize), 4))
			break;

		if (std::memcmp(chunkId, "fmt ", 4) == 0 && chunkSize >= 16) {
			f.read(reinterpret_cast<char *>(&audioFormat), 2);
			f.read(reinterpret_cast<char *>(&numChannels), 2);
			f.read(reinterpret_cast<char *>(&sampleRate), 4);
			f.seekg(6, std::ios::cur); // skip byteRate and blockAlign
			f.read(reinterpret_cast<char *>(&bitsPerSample), 2);
			if (chunkSize > 16)
				f.seekg(chunkSize - 16, std::ios::cur);
		} else if (std::memcmp(chunkId, "data", 4) == 0) {
			dataBuffer.resize(chunkSize);
			f.read(dataBuffer.data(), chunkSize);
			break;
		} else {
			f.seekg(chunkSize, std::ios::cur);
		}
	}

	if (dataBuffer.empty() || sampleRate == 0 || numChannels == 0) {
		std::cerr << "Corrupted WAV or missing data chunk\n";
		return false;
	}

	if (audioFormat == 1 && bitsPerSample == 16) {
		const int16_t *pcm = reinterpret_cast<const int16_t *>(dataBuffer.data());
		size_t totalWords = dataBuffer.size() / sizeof(int16_t);
		size_t nSamples = totalWords / numChannels;
		outSamples.reserve(nSamples);
		for (size_t i = 0; i < nSamples; ++i) {
			float sum = 0.0f;
			for (uint16_t ch = 0; ch < numChannels; ++ch) {
				sum += pcm[i * numChannels + ch] / 32768.0f;
			}
			outSamples.push_back(sum / numChannels);
		}
	} else if (audioFormat == 3 && bitsPerSample == 32) {
		const float *pcm = reinterpret_cast<const float *>(dataBuffer.data());
		size_t nSamples = dataBuffer.size() / (sizeof(float) * numChannels);
		outSamples.reserve(nSamples);
		for (size_t i = 0; i < nSamples; ++i) {
			float sum = 0.0f;
			for (uint16_t ch = 0; ch < numChannels; ++ch) {
				sum += pcm[i * numChannels + ch];
			}
			outSamples.push_back(sum / numChannels);
		}
	} else {
		std::cerr << "Unsupported WAV format: format=" << audioFormat << ", bits=" << bitsPerSample << "\n";
		return false;
	}

	return true;
}

struct DetectedReference {
	std::string label;
	int confidence = 0;
	std::string status;
	std::string kind;
	double timestampSec = 0.0;
};

int main(int argc, char **argv)
{
	std::vector<std::string> posArgs;
	bool isRealtime = false;

	for (int i = 1; i < argc; ++i) {
		std::string arg = argv[i];
		if (arg == "--realtime" || arg == "-r") {
			isRealtime = true;
		} else {
			posArgs.push_back(arg);
		}
	}

	std::string modelPath = (posArgs.size() > 0) ? posArgs[0] : "";
	std::string audioPath = (posArgs.size() > 1) ? posArgs[1] : "test-audio/pewbeam_demo_16k.wav";
	std::string tsvPath = (posArgs.size() > 2) ? posArgs[2] : "tests/kjv.tsv";

	if (modelPath.empty()) {
		const char *home = std::getenv("HOME");
		if (home) {
			std::string p = std::string(home) + "/Library/Application Support/obs-studio/plugin_config/ld-scripture/models/ggml-small.en-q5_1.bin";
			std::ifstream check(p);
			if (check.is_open())
				modelPath = p;
		}
	}

	if (modelPath.empty()) {
		std::cerr << "Usage: " << argv[0] << " <model_path> [audio_wav_path] [kjv_tsv_path] [--realtime]\n";
		return 1;
	}

	std::cout << "=========================================================\n";
	std::cout << "LD Worship & Word: Audio Pipeline Test (" << (isRealtime ? "REAL-TIME" : "BATCH") << ")\n";
	std::cout << "Model: " << modelPath << "\n";
	std::cout << "Audio: " << audioPath << "\n";
	std::cout << "TSV:   " << tsvPath << "\n";
	std::cout << "=========================================================\n";

	// 1. Load Bible TSV
	std::vector<ldsd::Verse> kjv;
	{
		std::ifstream f(tsvPath);
		if (!f.is_open()) {
			std::cerr << "Error: Cannot open Bible TSV: " << tsvPath << "\n";
			return 1;
		}
		std::string line;
		while (std::getline(f, line)) {
			std::istringstream s(line);
			ldsd::Verse v;
			std::string c, n;
			std::getline(s, v.book, '\t');
			std::getline(s, c, '\t');
			std::getline(s, n, '\t');
			std::getline(s, v.text);
			if (!c.empty() && !n.empty()) {
				v.chapter = std::stoi(c);
				v.verse = std::stoi(n);
				kjv.push_back(v);
			}
		}
	}
	std::cout << "Loaded " << kjv.size() << " Bible verses.\n";

	ldsd::LiveScripture live(&kjv);

	// 2. Load WAV
	std::vector<float> audio;
	if (!loadWav16kMono(audioPath, audio)) {
		return 1;
	}
	double duration = static_cast<double>(audio.size()) / 16000.0;
	std::cout << "Loaded WAV: " << audio.size() << " samples (" << duration << "s)\n";

	// 3. Init Whisper with GPU + Flash Attention on Apple
	whisper_context_params cparams = whisper_context_default_params();
#ifdef __APPLE__
	cparams.use_gpu = true;
	cparams.flash_attn = true;
#endif
	whisper_context *ctx = whisper_init_from_file_with_params(modelPath.c_str(), cparams);
	if (!ctx) {
		std::cerr << "Error: Failed to initialize Whisper from: " << modelPath << "\n";
		return 1;
	}

	// 4. Run Streaming Pipeline
	std::vector<float> streamBuffer;
	double bufStartSec = 0.0;
	double lastCommittedTimeSec = -1.0;
	double lastCommittedSentenceEndSec = -1.0;
	std::vector<TimedWord> prevPassWords;
	std::string committedHistory;

	struct DetectionRecord {
		std::string label;
		int confidence;
		std::string status;
		std::string kind;
		double streamTimeSec;
		double wallTimeSec;
	};
	std::vector<DetectionRecord> allDetections;

	constexpr size_t kChunkSize = 16000; // 1.0s hop
	double streamNow = 0.0;

	auto pipelineStartTime = std::chrono::steady_clock::now();
	auto wallStartTime = pipelineStartTime;

	int passCount = 0;
	double totalPassTimeMs = 0.0;
	double maxPassTimeMs = 0.0;
	double maxBacklogSec = 0.0;
	int lastReported10s = 0;
	int guardActivations = 0;
	double totalBufferDur = 0.0;
	int bufferSampleCount = 0;
	double maxBufferDur = 0.0;
	double backlogOver3sStart = -1.0;
	bool fallingBehindAlert = false;
	size_t totalAudioProcessedSamples = 0;

	size_t pos = 0;
	while (pos < audio.size()) {
		size_t targetPos;
		if (isRealtime) {
			auto now = std::chrono::steady_clock::now();
			double wallElapsed = std::chrono::duration<double>(now - wallStartTime).count();
			targetPos = std::min(audio.size(), static_cast<size_t>(wallElapsed * 16000.0));
			if (targetPos <= pos) {
				double nextChunkTime = (static_cast<double>(pos + kChunkSize)) / 16000.0;
				if (nextChunkTime > wallElapsed) {
					std::this_thread::sleep_for(std::chrono::duration<double>(nextChunkTime - wallElapsed));
				}
				auto nowAfter = std::chrono::steady_clock::now();
				double wallAfter = std::chrono::duration<double>(nowAfter - wallStartTime).count();
				targetPos = std::min(audio.size(), static_cast<size_t>(wallAfter * 16000.0));
			}
		} else {
			targetPos = std::min(audio.size(), pos + kChunkSize);
		}

		if (targetPos <= pos) {
			targetPos = std::min(audio.size(), pos + kChunkSize);
		}

		size_t arrivingSamples = targetPos - pos;
		double arrivingSec = static_cast<double>(arrivingSamples) / 16000.0;
		streamNow = static_cast<double>(targetPos) / 16000.0;

		if (isRealtime && arrivingSamples > static_cast<size_t>(1.25 * 16000)) {
			guardActivations++;
			double wallNow = std::chrono::duration<double>(std::chrono::steady_clock::now() - wallStartTime).count();
			double curBacklog = std::max(0.0, wallNow - (static_cast<double>(pos) / 16000.0));
			printf("  [BACKLOG GUARD] Activated: backlog=%.2fs (arrived %.2fs of audio), skipping interim passes to process newest full buffer\n",
			       curBacklog, arrivingSec);
		}

		if (streamBuffer.empty()) {
			bufStartSec = static_cast<double>(pos) / 16000.0;
		}
		streamBuffer.insert(streamBuffer.end(), audio.begin() + pos, audio.begin() + targetPos);
		totalAudioProcessedSamples += arrivingSamples;
		pos = targetPos;

		if (streamBuffer.size() < static_cast<size_t>(2.5 * 16000))
			continue;

		passCount++;
		auto tPassStart = std::chrono::steady_clock::now();

		// VAD: Speech presence check
		auto tVadStart = std::chrono::steady_clock::now();
		float maxRms = 0.0f;
		constexpr size_t subSize = 3200;
		for (size_t i = 0; i + subSize <= streamBuffer.size(); i += subSize) {
			float sum = 0.0f;
			for (size_t j = 0; j < subSize; ++j) {
				float s = streamBuffer[i + j];
				sum += s * s;
			}
			float subRms = std::sqrt(sum / static_cast<float>(subSize));
			if (subRms > maxRms)
				maxRms = subRms;
		}
		auto tVadEnd = std::chrono::steady_clock::now();
		double vadMs = std::chrono::duration<double, std::milli>(tVadEnd - tVadStart).count();

		if (maxRms < 0.002f) {
			auto tPassEnd = std::chrono::steady_clock::now();
			double passMs = std::chrono::duration<double, std::milli>(tPassEnd - tPassStart).count();
			totalPassTimeMs += passMs;
			maxPassTimeMs = std::max(maxPassTimeMs, passMs);
			printf("[pass %2d: buf=%4.1fs, total=%5.1fms (vad=%4.1fms, whisper=0.0ms [silent], other=0.0ms)]\n",
			       passCount, static_cast<double>(streamBuffer.size()) / 16000.0, passMs, vadMs);
			continue;
		}

		double bufStart = bufStartSec;
		std::vector<float> audioCopy = streamBuffer;
		double bufferSeconds = static_cast<double>(audioCopy.size()) / 16000.0;
		totalBufferDur += bufferSeconds;
		bufferSampleCount++;
		maxBufferDur = std::max(maxBufferDur, bufferSeconds);

		// 80Hz high-pass and AGC
		applyHighPass80Hz(audioCopy, 16000.0f);
		applyAgcTo20dBFS(audioCopy);

		// Whisper params
		whisper_full_params wparams = whisper_full_default_params(WHISPER_SAMPLING_GREEDY);
		wparams.strategy = WHISPER_SAMPLING_GREEDY;
		wparams.temperature = 0.0f;
		wparams.temperature_inc = 0.0f;
		int maxTokens = std::max(64, static_cast<int>(std::ceil(bufferSeconds * 12.0)));
		wparams.max_tokens = maxTokens;
		wparams.token_timestamps = true;
		wparams.no_timestamps = false;
		wparams.single_segment = false;
		wparams.suppress_blank = true;
		wparams.no_speech_thold = 0.6f;
		wparams.print_progress = false;
		wparams.print_special = false;
		wparams.print_realtime = false;
		wparams.print_timestamps = false;
		wparams.language = "en";
		wparams.translate = false;
		wparams.no_context = true;
		wparams.n_threads = std::clamp(static_cast<int>(std::thread::hardware_concurrency()), 2, 8);

		// Size audio_ctx to the buffer (~buffer_seconds/30*1500, multiple of 64, min 384, max 1500)
		int targetCtx = static_cast<int>(std::ceil((bufferSeconds / 30.0) * 1500.0));
		targetCtx = ((targetCtx + 63) / 64) * 64;
		wparams.audio_ctx = std::clamp(targetCtx, 384, 1500);

		std::string prompt = "A sermon. The preacher reads from the Bible.";
		if (!committedHistory.empty()) {
			size_t take = std::min(committedHistory.size(), static_cast<size_t>(200));
			prompt += " " + committedHistory.substr(committedHistory.size() - take);
		}
		wparams.initial_prompt = prompt.c_str();

		auto tWhisperStart = std::chrono::steady_clock::now();
		int ret = whisper_full(ctx, wparams, audioCopy.data(), static_cast<int>(audioCopy.size()));
		auto tWhisperEnd = std::chrono::steady_clock::now();
		double whisperMs = std::chrono::duration<double, std::milli>(tWhisperEnd - tWhisperStart).count();

		if (ret != 0) {
			auto tPassEnd = std::chrono::steady_clock::now();
			double passMs = std::chrono::duration<double, std::milli>(tPassEnd - tPassStart).count();
			totalPassTimeMs += passMs;
			maxPassTimeMs = std::max(maxPassTimeMs, passMs);
			continue;
		}

		// Extract words with token timestamps
		std::vector<TimedWord> words;
		int n_segments = whisper_full_n_segments(ctx);
		for (int s = 0; s < n_segments; ++s) {
			float noSpeechProb = whisper_full_get_segment_no_speech_prob(ctx, s);
			if (noSpeechProb > 0.6f)
				continue;

			const char *segTextRaw = whisper_full_get_segment_text(ctx, s);
			std::string segText = segTextRaw ? segTextRaw : "";
			if (isStockSilence(segText) || isMostlyPrompt(segText))
				continue;

			std::vector<TimedWord> segWords;
			int n_tokens = whisper_full_n_tokens(ctx, s);
			whisper_token eotToken = whisper_token_eot(ctx);
			for (int t = 0; t < n_tokens; ++t) {
				whisper_token id = whisper_full_get_token_id(ctx, s, t);
				if (id >= eotToken)
					continue;

				const char *txt = whisper_full_get_token_text(ctx, s, t);
				if (!txt || txt[0] == '\0')
					continue;
				std::string tokenStr(txt);
				if (tokenStr.rfind("[_", 0) == 0 || tokenStr.rfind("<|", 0) == 0)
					continue;

				whisper_token_data data = whisper_full_get_token_data(ctx, s, t);

				bool isStartOfWord = (tokenStr[0] == ' ' || tokenStr[0] == '\t' || tokenStr[0] == '\n');
				if (isStartOfWord || segWords.empty()) {
					std::string w = isStartOfWord ? tokenStr.substr(1) : tokenStr;
					if (!w.empty()) {
						double t0 = bufStart + (static_cast<double>(data.t0) * 0.01);
						double t1 = bufStart + (static_cast<double>(data.t1) * 0.01);
						segWords.push_back({w, t0, t1});
					}
				} else {
					segWords.back().text += tokenStr;
					double t1 = bufStart + (static_cast<double>(data.t1) * 0.01);
					segWords.back().endTimeSec = std::max(segWords.back().endTimeSec, t1);
				}
			}

			if (hasRepetitionLoop(segWords))
				continue;

			words.insert(words.end(), segWords.begin(), segWords.end());
		}

		if (hasRepetitionLoop(words))
			words.clear();

		// LocalAgreement-2: filter committed words
		std::vector<TimedWord> uncommitted = getUncommittedWords(words, committedHistory, lastCommittedTimeSec);

		size_t prefixLen = 0;
		while (prefixLen < uncommitted.size() && prefixLen < prevPassWords.size()) {
			if (cleanWord(uncommitted[prefixLen].text) == cleanWord(prevPassWords[prefixLen].text)) {
				prefixLen++;
			} else {
				break;
			}
		}

		int64_t nowMs = static_cast<int64_t>(streamNow * 1000.0);
		double nowWallSec = std::chrono::duration<double>(std::chrono::steady_clock::now() - wallStartTime).count();

		// 1. Committed words
		if (prefixLen > 0) {
			std::string committedText;
			for (size_t i = 0; i < prefixLen; ++i) {
				if (i > 0)
					committedText += " ";
				committedText += uncommitted[i].text;
				if (isSentenceEnd(uncommitted[i].text)) {
					lastCommittedSentenceEndSec = uncommitted[i].endTimeSec;
				}
			}
			lastCommittedTimeSec = uncommitted[prefixLen - 1].endTimeSec;
			committedHistory += " " + committedText;
			if (committedHistory.size() > 1000)
				committedHistory = committedHistory.substr(committedHistory.size() - 800);

			printf("[%5.1fs] COMMITTED: \"%s\"\n", streamNow, committedText.c_str());

			auto refs = live.push(committedText, true, nowMs);
			for (const auto &r : refs) {
				int conf = static_cast<int>(std::round(r.confidence * 100.0));
				printf("[%5.1fs]   >>> DETECTED: %s (status=%s, kind=%s, conf=%d%%)\n",
				       streamNow, r.label.c_str(), r.status.c_str(), r.kind.c_str(), conf);
				allDetections.push_back({r.label, conf, r.status, r.kind, streamNow, nowWallSec});
			}
		}

		// 2. Interim words
		if (prefixLen < uncommitted.size()) {
			std::string interimText;
			for (size_t i = prefixLen; i < uncommitted.size(); ++i) {
				if (!interimText.empty())
					interimText += " ";
				interimText += uncommitted[i].text;
			}

			auto refs = live.push(interimText, false, nowMs);
			for (const auto &r : refs) {
				int conf = static_cast<int>(std::round(r.confidence * 100.0));
				printf("[%5.1fs]   (tentative: %s, %s, conf=%d%%)\n",
				       streamNow, r.label.c_str(), r.kind.c_str(), conf);
				allDetections.push_back({r.label, conf, r.status, r.kind, streamNow, nowWallSec});
			}

			prevPassWords = std::vector<TimedWord>(uncommitted.begin() + prefixLen, uncommitted.end());
		} else {
			prevPassWords.clear();
		}

		// 3. SAFER TRIM POINTS: when buffer > 6s, trim at the latest COMMITTED word that is
		// followed by a pause of at least 0.3s (gap to next word's start), or at . ? ! if that comes first.
		// Never trim inside a run of words with no pause.
		double bufDur = static_cast<double>(streamBuffer.size()) / 16000.0;
		if (bufDur > 6.0) {
			int bestTrimWordIdx = -1;
			for (int i = static_cast<int>(words.size()) - 1; i >= 0; --i) {
				if (words[i].endTimeSec > lastCommittedTimeSec)
					continue;
				if (words[i].endTimeSec <= bufStartSec)
					continue;

				bool isPunct = isSentenceEnd(words[i].text);
				bool hasPause = false;
				if (i + 1 < static_cast<int>(words.size())) {
					double gap = words[i + 1].startTimeSec - words[i].endTimeSec;
					if (gap >= 0.3)
						hasPause = true;
				} else {
					double gap = (bufStartSec + bufDur) - words[i].endTimeSec;
					if (gap >= 0.3)
						hasPause = true;
				}

				if (isPunct || hasPause) {
					bestTrimWordIdx = i;
					break;
				}
			}

			if (bestTrimWordIdx >= 0) {
				double trimSec = words[bestTrimWordIdx].endTimeSec - bufStartSec;
				size_t drop = std::min(streamBuffer.size(), static_cast<size_t>(trimSec * 16000.0));
				if (drop > 0) {
					streamBuffer.erase(streamBuffer.begin(), streamBuffer.begin() + drop);
					bufStartSec += static_cast<double>(drop) / 16000.0;
				}
			}
		}

		// 4. LOWER HARD CAP: 10s instead of 15s. When the cap forces a trim, first FORCE-COMMIT
		// the current hypothesis words up to the trim point (push them as final) so no words are lost.
		bufDur = static_cast<double>(streamBuffer.size()) / 16000.0;
		if (bufDur > 10.0) {
			int forceIdx = -1;

			// Look for latest word followed by pause >= 0.3s or ending in . ? !
			for (int i = static_cast<int>(uncommitted.size()) - 1; i >= 0; --i) {
				bool isPunct = isSentenceEnd(uncommitted[i].text);
				bool hasPause = false;
				if (i + 1 < static_cast<int>(uncommitted.size())) {
					double gap = uncommitted[i + 1].startTimeSec - uncommitted[i].endTimeSec;
					if (gap >= 0.3)
						hasPause = true;
				} else {
					double gap = (bufStartSec + bufDur) - uncommitted[i].endTimeSec;
					if (gap >= 0.3)
						hasPause = true;
				}
				if (isPunct || hasPause) {
					forceIdx = i;
					break;
				}
			}

			// If no 0.3s pause or punct, look for largest gap
			if (forceIdx < 0 && uncommitted.size() > 1) {
				double maxGap = 0.0;
				int bestGapIdx = -1;
				for (size_t i = 0; i + 1 < uncommitted.size(); ++i) {
					double gap = uncommitted[i + 1].startTimeSec - uncommitted[i].endTimeSec;
					if (gap > maxGap) {
						maxGap = gap;
						bestGapIdx = static_cast<int>(i);
					}
				}
				if (maxGap >= 0.15 && bestGapIdx >= 0) {
					forceIdx = bestGapIdx;
				}
			}

			// If still nothing, force-commit all uncommitted words so no phrase is cut in half
			if (forceIdx < 0 && !uncommitted.empty()) {
				forceIdx = static_cast<int>(uncommitted.size()) - 1;
			}

			if (forceIdx >= 0) {
				std::string forceText;
				for (int i = 0; i <= forceIdx; ++i) {
					if (i > 0)
						forceText += " ";
					forceText += uncommitted[i].text;
				}
				auto refs = live.push(forceText, true, nowMs);
				for (const auto &r : refs) {
					int conf = static_cast<int>(std::round(r.confidence * 100.0));
					printf("[%5.1fs]   >>> DETECTED (force-commit): %s (status=%s, kind=%s, conf=%d%%)\n",
					       streamNow, r.label.c_str(), r.status.c_str(), r.kind.c_str(), conf);
					allDetections.push_back({r.label, conf, r.status, r.kind, streamNow, nowWallSec});
				}
				committedHistory += " " + forceText;
				if (committedHistory.size() > 1000)
					committedHistory = committedHistory.substr(committedHistory.size() - 800);

				lastCommittedTimeSec = uncommitted[forceIdx].endTimeSec;
				double trimToSec = uncommitted[forceIdx].endTimeSec;
				if (trimToSec > bufStartSec) {
					double trimSec = trimToSec - bufStartSec;
					size_t drop = std::min(streamBuffer.size(), static_cast<size_t>(trimSec * 16000.0));
					if (drop > 0) {
						streamBuffer.erase(streamBuffer.begin(), streamBuffer.begin() + drop);
						bufStartSec += static_cast<double>(drop) / 16000.0;
					}
				}
				prevPassWords.clear();
			}
		}

		auto tPassEnd = std::chrono::steady_clock::now();
		double passMs = std::chrono::duration<double, std::milli>(tPassEnd - tPassStart).count();
		totalPassTimeMs += passMs;
		maxPassTimeMs = std::max(maxPassTimeMs, passMs);
		double otherMs = std::max(0.0, passMs - vadMs - whisperMs);

		printf("[pass %2d: buf=%4.1fs, total=%5.1fms (vad=%4.1fms, whisper=%5.1fms, other=%4.1fms)]\n",
		       passCount, bufferSeconds, passMs, vadMs, whisperMs, otherMs);

		// Backlog tracking and alert check
		if (isRealtime) {
			auto nowWall = std::chrono::steady_clock::now();
			double wallElapsed = std::chrono::duration<double>(nowWall - wallStartTime).count();
			double curBacklog = std::max(0.0, wallElapsed - streamNow);
			if (curBacklog > maxBacklogSec)
				maxBacklogSec = curBacklog;

			if (curBacklog > 3.0) {
				if (backlogOver3sStart < 0.0) {
					backlogOver3sStart = wallElapsed;
				} else if (wallElapsed - backlogOver3sStart > 5.0 && !fallingBehindAlert) {
					fallingBehindAlert = true;
					printf("  ⚠️ [ALERT] Processing falling behind (backlog > 3.0s for > 5.0s, cur=%.2fs)\n", curBacklog);
				}
			} else {
				backlogOver3sStart = -1.0;
				fallingBehindAlert = false;
			}

			int check10 = static_cast<int>(streamNow / 10.0);
			if (check10 > lastReported10s && check10 <= 5) {
				lastReported10s = check10;
				printf("  [BACKLOG @ %ds]: %4.2fs (max so far: %4.2fs)\n",
				       check10 * 10, curBacklog, maxBacklogSec);
			}
		}
	}

	whisper_free(ctx);

	double realTimeFactor = (totalPassTimeMs / 1000.0) / duration;
	double avgPassMs = passCount > 0 ? (totalPassTimeMs / passCount) : 0.0;
	double avgBufferDur = bufferSampleCount > 0 ? (totalBufferDur / bufferSampleCount) : 0.0;
	size_t droppedAudioSamples = audio.size() > totalAudioProcessedSamples ? (audio.size() - totalAudioProcessedSamples) : 0;

	std::cout << "\n---------------------------------------------------------\n";
	std::cout << "Timing & Pipeline Summary:\n";
	std::cout << "  Audio duration:       " << duration << " s\n";
	std::cout << "  Total processing:     " << (totalPassTimeMs / 1000.0) << " s\n";
	std::cout << "  Real-time factor:     " << realTimeFactor << "x ("
		  << (realTimeFactor < 1.0 ? "FASTER than real-time" : "SLOWER than real-time") << ")\n";
	std::cout << "  Pass count:           " << passCount << "\n";
	std::cout << "  Average ms per pass:  " << avgPassMs << " ms\n";
	std::cout << "  Maximum ms per pass:  " << maxPassTimeMs << " ms\n";
	std::cout << "  Average buffer:       " << avgBufferDur << " s\n";
	std::cout << "  Maximum buffer:       " << maxBufferDur << " s\n";
	if (isRealtime) {
		std::cout << "  Maximum backlog:      " << maxBacklogSec << " s\n";
		std::cout << "  Guard activations:    " << guardActivations << "\n";
	}
	std::cout << "  Audio dropped:        " << (static_cast<double>(droppedAudioSamples) / 16000.0)
		  << " s (" << (droppedAudioSamples == 0 ? "CONFIRMED: NO AUDIO DROPPED" : "WARNING: AUDIO WAS DROPPED") << ")\n";
	std::cout << "---------------------------------------------------------\n";

	// Verification against expected results:
	//   1.6 s   Psalms 27:1
	//  13.9 s   Psalms 139:23
	//  28.7 s   Psalms 138:7
	//  43.0 s   Psalms 138:8
	//  No other confirmed references.

	struct ExpectedCheck {
		std::string label;
		double spokenTimeSec;
		bool detected = false;
		double detectedStreamSec = 0.0;
		double detectedWallSec = 0.0;
		std::string detectedStatus;
		int confidence = 0;
	};

	std::vector<ExpectedCheck> expected = {
		{"Psalms 27:1", 1.6},
		{"Psalms 139:23", 13.9},
		{"Psalms 138:7", 28.7},
		{"Psalms 138:8", 43.0},
	};

	std::vector<std::string> unexpectedConfirmed;

	for (const auto &d : allDetections) {
		for (auto &exp : expected) {
			if (d.label == exp.label) {
				if (!exp.detected || (exp.detectedStatus == "preview" && d.status != "preview")) {
					exp.detected = true;
					exp.detectedStreamSec = d.streamTimeSec;
					exp.detectedWallSec = d.wallTimeSec;
					exp.detectedStatus = d.status;
					exp.confidence = d.confidence;
				}
			}
		}

		if (d.status != "preview") {
			bool isExpected = false;
			for (const auto &exp : expected) {
				if (d.label == exp.label || exp.label.find(d.label + ":") == 0 || exp.label == d.label) {
					isExpected = true;
					break;
				}
			}
			if (!isExpected) {
				if (std::find(unexpectedConfirmed.begin(), unexpectedConfirmed.end(), d.label) == unexpectedConfirmed.end()) {
					unexpectedConfirmed.push_back(d.label);
				}
			}
		}
	}

	std::cout << "\n=================== VERSE DETECTION RESULTS ===================\n";
	bool allPassed = true;
	for (const auto &exp : expected) {
		if (exp.detected) {
			double delay = isRealtime ? (exp.detectedWallSec - exp.spokenTimeSec)
						  : (exp.detectedStreamSec - exp.spokenTimeSec);
			bool isLate = delay > 2.0;
			std::cout << "  [" << (isLate ? "LATE" : "PASS") << "] " << exp.label
				  << " | spoken: " << exp.spokenTimeSec << "s"
				  << " | detected: " << (isRealtime ? exp.detectedWallSec : exp.detectedStreamSec) << "s"
				  << " | delay: " << delay << "s (" << (isLate ? "LATE (> 2s)" : "ON TIME") << ")"
				  << " | status: " << exp.detectedStatus
				  << " | conf: " << exp.confidence << "%\n";
		} else {
			std::cout << "  [FAIL] " << exp.label << " (spoken at " << exp.spokenTimeSec << "s) was NOT detected!\n";
			allPassed = false;
		}
	}

	if (!unexpectedConfirmed.empty()) {
		std::cout << "  [FAIL] Unexpected confirmed detections found:\n";
		for (const auto &u : unexpectedConfirmed) {
			std::cout << "         - " << u << "\n";
		}
		allPassed = false;
	} else {
		std::cout << "  [PASS] No unexpected confirmed references.\n";
	}
	std::cout << "===============================================================\n";

	return allPassed ? 0 : 1;
}
