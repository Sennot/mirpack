#include "smart_contrast.hpp"

#include <Geode/Geode.hpp>
#include <Geode/binding/GameLevelManager.hpp>
#include <Geode/binding/LocalLevelManager.hpp>
#include <Geode/loader/SettingV3.hpp>
#include <Geode/ui/NineSlice.hpp>
#include <Geode/ui/Notification.hpp>
#include <Geode/ui/Popup.hpp>
#include <Geode/ui/ScrollLayer.hpp>
#include <Geode/ui/TextInput.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using namespace geode::prelude;

namespace cleanfeed::smart_contrast {
    namespace {
        constexpr std::array<std::string_view, 8> kColorSettingKeys = {
            "solid-color",
            "hazard-color",
            "interactable-color",
            "player-color",
            "player-inner-color",
            "player-rotated-color",
            "trajectory-hold-color",
            "trajectory-release-color",
        };

        struct Rgb {
            uint8_t r = 255;
            uint8_t g = 255;
            uint8_t b = 255;
        };

        struct Lab {
            double l = 0.0;
            double a = 0.0;
            double b = 0.0;
        };

        struct Palette {
            std::array<Rgb, kColorSettingKeys.size()> colors;
        };

        struct AnalysisResult {
            std::array<Palette, 3> palettes;
            size_t objectCount = 0;
            size_t sampledColors = 0;
        };

        struct LevelEntry {
            Ref<GJGameLevel> level;
            std::string name;
            int id = 0;
            bool editorLevel = false;
        };

        struct AnalysisJob {
            std::atomic<float> progress = 0.f;
            std::atomic<int> stage = 0;
            std::atomic<bool> cancelled = false;
            std::atomic<bool> complete = false;
            std::mutex resultMutex;
            std::optional<AnalysisResult> result;
        };

        struct WeightedColor {
            Rgb rgb;
            double weight = 0.0;
        };

        struct Histogram {
            std::array<double, 32 * 32 * 32> bins{};
            size_t samples = 0;

            void add(Rgb color, double weight) {
                if (weight <= 0.0) return;
                auto const index =
                    (static_cast<size_t>(color.r >> 3) << 10) |
                    (static_cast<size_t>(color.g >> 3) << 5) |
                    static_cast<size_t>(color.b >> 3);
                bins[index] += weight;
                ++samples;
            }

            std::vector<WeightedColor> dominant(size_t limit) const {
                std::vector<WeightedColor> colors;
                colors.reserve(bins.size());

                for (size_t index = 0; index < bins.size(); ++index) {
                    auto const weight = bins[index];
                    if (weight <= 0.0) continue;

                    colors.push_back({
                        {
                            static_cast<uint8_t>((((index >> 10) & 31) << 3) + 4),
                            static_cast<uint8_t>((((index >> 5) & 31) << 3) + 4),
                            static_cast<uint8_t>(((index & 31) << 3) + 4),
                        },
                        weight,
                    });
                }

                auto const count = std::min(limit, colors.size());
                std::partial_sort(
                    colors.begin(), colors.begin() + count, colors.end(),
                    [](auto const& lhs, auto const& rhs) {
                        return lhs.weight > rhs.weight;
                    }
                );
                colors.resize(count);
                return colors;
            }
        };

        double srgbToLinear(double value) {
            value /= 255.0;
            return value <= 0.04045
                ? value / 12.92
                : std::pow((value + 0.055) / 1.055, 2.4);
        }

        double linearToSrgb(double value) {
            value = std::clamp(value, 0.0, 1.0);
            auto const srgb = value <= 0.0031308
                ? value * 12.92
                : 1.055 * std::pow(value, 1.0 / 2.4) - 0.055;
            return srgb * 255.0;
        }

        double luminance(Rgb color) {
            return
                0.2126 * srgbToLinear(color.r) +
                0.7152 * srgbToLinear(color.g) +
                0.0722 * srgbToLinear(color.b);
        }

        double contrastRatio(Rgb lhs, Rgb rhs) {
            auto const first = luminance(lhs);
            auto const second = luminance(rhs);
            auto const bright = std::max(first, second);
            auto const dark = std::min(first, second);
            return (bright + 0.05) / (dark + 0.05);
        }

        Lab toLab(Rgb color) {
            auto const r = srgbToLinear(color.r);
            auto const g = srgbToLinear(color.g);
            auto const b = srgbToLinear(color.b);

            auto const l = std::cbrt(0.4122214708 * r + 0.5363325363 * g + 0.0514459929 * b);
            auto const m = std::cbrt(0.2119034982 * r + 0.6806995451 * g + 0.1073969566 * b);
            auto const s = std::cbrt(0.0883024619 * r + 0.2817188376 * g + 0.6299787005 * b);

            return {
                0.2104542553 * l + 0.7936177850 * m - 0.0040720468 * s,
                1.9779984951 * l - 2.4285922050 * m + 0.4505937099 * s,
                0.0259040371 * l + 0.7827717662 * m - 0.8086757660 * s,
            };
        }

        Rgb fromLch(double lightness, double chroma, double hueDegrees) {
            constexpr double pi = 3.14159265358979323846;
            auto const hue = hueDegrees * pi / 180.0;
            auto const a = std::cos(hue) * chroma;
            auto const b = std::sin(hue) * chroma;

            auto const lRoot = lightness + 0.3963377774 * a + 0.2158037573 * b;
            auto const mRoot = lightness - 0.1055613458 * a - 0.0638541728 * b;
            auto const sRoot = lightness - 0.0894841775 * a - 1.2914855480 * b;
            auto const l = lRoot * lRoot * lRoot;
            auto const m = mRoot * mRoot * mRoot;
            auto const s = sRoot * sRoot * sRoot;

            auto const r = 4.0767416621 * l - 3.3077115913 * m + 0.2309699292 * s;
            auto const g = -1.2684380046 * l + 2.6097574011 * m - 0.3413193965 * s;
            auto const blue = -0.0041960863 * l - 0.7034186147 * m + 1.7076147010 * s;

            return {
                static_cast<uint8_t>(std::lround(linearToSrgb(r))),
                static_cast<uint8_t>(std::lround(linearToSrgb(g))),
                static_cast<uint8_t>(std::lround(linearToSrgb(blue))),
            };
        }

        Rgb gamutMappedLch(double lightness, double chroma, double hueDegrees) {
            // Clamping highly saturated OKLCH colors can collapse different hues
            // into the same RGB corner. Reducing chroma first keeps the patterns distinct.
            for (auto current = chroma; current >= 0.04; current -= 0.01) {
                constexpr double pi = 3.14159265358979323846;
                auto const hue = hueDegrees * pi / 180.0;
                auto const a = std::cos(hue) * current;
                auto const b = std::sin(hue) * current;
                auto const lRoot = lightness + 0.3963377774 * a + 0.2158037573 * b;
                auto const mRoot = lightness - 0.1055613458 * a - 0.0638541728 * b;
                auto const sRoot = lightness - 0.0894841775 * a - 1.2914855480 * b;
                auto const l = lRoot * lRoot * lRoot;
                auto const m = mRoot * mRoot * mRoot;
                auto const s = sRoot * sRoot * sRoot;
                auto const r = 4.0767416621 * l - 3.3077115913 * m + 0.2309699292 * s;
                auto const g = -1.2684380046 * l + 2.6097574011 * m - 0.3413193965 * s;
                auto const blue = -0.0041960863 * l - 0.7034186147 * m + 1.7076147010 * s;

                if (r >= 0.0 && r <= 1.0 && g >= 0.0 && g <= 1.0 && blue >= 0.0 && blue <= 1.0) {
                    return fromLch(lightness, current, hueDegrees);
                }
            }
            return fromLch(lightness, 0.04, hueDegrees);
        }

        double hueDistance(double first, double second) {
            auto distance = std::fmod(std::abs(first - second), 360.0);
            return std::min(distance, 360.0 - distance);
        }

        bool parseInt(std::string_view value, int& output) {
            if (value.empty()) return false;
            auto const begin = value.data();
            auto const end = value.data() + value.size();
            auto const result = std::from_chars(begin, end, output);
            return result.ec == std::errc{} && result.ptr == end;
        }

        template <class Callback>
        void visitPairs(std::string_view input, char separator, Callback&& callback) {
            size_t position = 0;
            while (position < input.size()) {
                auto keyEnd = input.find(separator, position);
                if (keyEnd == std::string_view::npos) break;
                auto valueEnd = input.find(separator, keyEnd + 1);
                if (valueEnd == std::string_view::npos) valueEnd = input.size();

                callback(
                    input.substr(position, keyEnd - position),
                    input.substr(keyEnd + 1, valueEnd - keyEnd - 1)
                );
                position = valueEnd + 1;
            }
        }

        std::unordered_map<int, Rgb> parseStartColors(std::string_view start, Histogram& histogram) {
            std::string_view encodedColors;
            visitPairs(start, ',', [&](std::string_view key, std::string_view value) {
                if (key == "kS38") encodedColors = value;
            });

            std::unordered_map<int, Rgb> channels;
            size_t position = 0;
            while (position < encodedColors.size()) {
                auto end = encodedColors.find('|', position);
                if (end == std::string_view::npos) end = encodedColors.size();
                auto const encoded = encodedColors.substr(position, end - position);

                int red = -1;
                int green = -1;
                int blue = -1;
                int targetRed = -1;
                int targetGreen = -1;
                int targetBlue = -1;
                int channel = -1;
                double opacity = 1.0;

                visitPairs(encoded, '_', [&](std::string_view keyText, std::string_view valueText) {
                    int key = 0;
                    int value = 0;
                    if (!parseInt(keyText, key)) return;

                    if (key == 7 || key == 15) {
                        try {
                            opacity = std::clamp(std::stod(std::string(valueText)), 0.0, 1.0);
                        } catch (...) {
                        }
                        return;
                    }
                    if (!parseInt(valueText, value)) return;

                    switch (key) {
                        case 1: red = value; break;
                        case 2: green = value; break;
                        case 3: blue = value; break;
                        case 6: channel = value; break;
                        case 11: targetRed = value; break;
                        case 12: targetGreen = value; break;
                        case 13: targetBlue = value; break;
                        default: break;
                    }
                });

                auto add = [&](int r, int g, int b, double weight) {
                    if (r < 0 || g < 0 || b < 0) return;
                    Rgb color {
                        static_cast<uint8_t>(std::clamp(r, 0, 255)),
                        static_cast<uint8_t>(std::clamp(g, 0, 255)),
                        static_cast<uint8_t>(std::clamp(b, 0, 255)),
                    };
                    histogram.add(color, weight * std::max(opacity, 0.1));
                    if (channel >= 0) channels[channel] = color;
                };

                auto const baseWeight = channel == 1000 ? 180.0 :
                    (channel == 1001 || channel == 1009 ? 90.0 : 15.0);
                add(red, green, blue, baseWeight);
                add(targetRed, targetGreen, targetBlue, baseWeight * 0.35);

                position = end + 1;
            }
            return channels;
        }

        AnalysisResult analyzeLevel(std::string const& data, std::shared_ptr<AnalysisJob> const& job) {
            Histogram histogram;
            std::unordered_map<int, double> channelUse;

            job->stage.store(0, std::memory_order_relaxed);
            job->progress.store(0.02f, std::memory_order_relaxed);

            auto const headerEnd = data.find(';');
            auto const header = std::string_view(data).substr(0, headerEnd);
            auto const channels = parseStartColors(header, histogram);

            job->stage.store(1, std::memory_order_relaxed);
            job->progress.store(0.08f, std::memory_order_relaxed);

            size_t objectCount = 0;
            size_t position = headerEnd == std::string::npos ? data.size() : headerEnd + 1;
            size_t nextProgressAt = position;

            while (position < data.size() && !job->cancelled.load(std::memory_order_relaxed)) {
                auto end = data.find(';', position);
                if (end == std::string::npos) end = data.size();
                auto const object = std::string_view(data).substr(position, end - position);

                int red = -1;
                int green = -1;
                int blue = -1;
                int targetChannel = -1;
                int mainChannel = -1;
                int secondaryChannel = -1;

                visitPairs(object, ',', [&](std::string_view keyText, std::string_view valueText) {
                    int key = 0;
                    int value = 0;
                    if (!parseInt(keyText, key) || !parseInt(valueText, value)) return;

                    switch (key) {
                        case 7: red = value; break;
                        case 8: green = value; break;
                        case 9: blue = value; break;
                        case 21: mainChannel = value; break;
                        case 22: secondaryChannel = value; break;
                        case 23: targetChannel = value; break;
                        default: break;
                    }
                });

                if (red >= 0 && green >= 0 && blue >= 0) {
                    histogram.add({
                        static_cast<uint8_t>(std::clamp(red, 0, 255)),
                        static_cast<uint8_t>(std::clamp(green, 0, 255)),
                        static_cast<uint8_t>(std::clamp(blue, 0, 255)),
                    }, targetChannel == 1000 ? 18.0 : 5.0);
                }
                if (mainChannel >= 0) channelUse[mainChannel] += 1.0;
                if (secondaryChannel >= 0) channelUse[secondaryChannel] += 0.55;

                ++objectCount;
                position = end + 1;

                if (position >= nextProgressAt) {
                    auto const fraction = data.empty()
                        ? 1.0
                        : static_cast<double>(position) / static_cast<double>(data.size());
                    job->progress.store(static_cast<float>(0.08 + 0.62 * fraction), std::memory_order_relaxed);
                    nextProgressAt = position + 65536;
                }
            }

            for (auto const& [channel, usage] : channelUse) {
                if (auto found = channels.find(channel); found != channels.end()) {
                    histogram.add(found->second, std::min(220.0, 2.0 * std::sqrt(usage)));
                }
            }

            if (histogram.samples == 0) {
                histogram.add({24, 24, 32}, 1.0);
                histogram.add({96, 96, 112}, 0.35);
            }

            job->stage.store(2, std::memory_order_relaxed);
            job->progress.store(0.76f, std::memory_order_relaxed);

            auto const dominant = histogram.dominant(96);
            double totalWeight = 0.0;
            double averageLuminance = 0.0;
            double averageA = 0.0;
            double averageB = 0.0;

            for (auto const& sample : dominant) {
                auto const lab = toLab(sample.rgb);
                totalWeight += sample.weight;
                averageLuminance += luminance(sample.rgb) * sample.weight;
                averageA += lab.a * sample.weight;
                averageB += lab.b * sample.weight;
            }
            if (totalWeight > 0.0) {
                averageLuminance /= totalWeight;
                averageA /= totalWeight;
                averageB /= totalWeight;
            }

            constexpr double pi = 3.14159265358979323846;
            auto dominantHue = std::atan2(averageB, averageA) * 180.0 / pi;
            if (dominantHue < 0.0) dominantHue += 360.0;
            auto const levelIsDark = averageLuminance < 0.38;

            auto visibilityScore = [&](Rgb candidate) {
                double weighted = 0.0;
                double weight = 0.0;
                double worstImportant = 21.0;
                auto const importantThreshold = totalWeight * 0.008;

                for (auto const& sample : dominant) {
                    auto const contrast = contrastRatio(candidate, sample.rgb);
                    weighted += std::min(contrast, 10.0) * sample.weight;
                    weight += sample.weight;
                    if (sample.weight >= importantThreshold) {
                        worstImportant = std::min(worstImportant, contrast);
                    }
                }
                auto const average = weight > 0.0 ? weighted / weight : 1.0;
                return 0.72 * average + 0.28 * std::min(worstImportant, 10.0);
            };

            auto buildPalette = [&](double lightness, double chroma, double preferredHue, bool maximum) {
                struct Candidate {
                    Rgb color;
                    double hue = 0.0;
                    double score = 0.0;
                };

                std::vector<Candidate> candidates;
                candidates.reserve(72);
                for (int hue = 0; hue < 360; hue += 5) {
                    auto const color = gamutMappedLch(lightness, chroma, static_cast<double>(hue));
                    auto const preference = (std::cos((hue - preferredHue) * pi / 180.0) + 1.0) * 0.18;
                    candidates.push_back({color, static_cast<double>(hue), visibilityScore(color) + preference});
                }
                std::sort(candidates.begin(), candidates.end(), [](auto const& lhs, auto const& rhs) {
                    return lhs.score > rhs.score;
                });

                std::array<Candidate, 3> selected{};
                size_t selectedCount = 0;
                for (auto const& candidate : candidates) {
                    bool separated = true;
                    for (size_t index = 0; index < selectedCount; ++index) {
                        if (hueDistance(candidate.hue, selected[index].hue) < 62.0) {
                            separated = false;
                            break;
                        }
                    }
                    if (!separated) continue;
                    selected[selectedCount++] = candidate;
                    if (selectedCount == selected.size()) break;
                }

                if (selectedCount < selected.size()) {
                    for (; selectedCount < selected.size(); ++selectedCount) {
                        selected[selectedCount] = candidates[selectedCount % candidates.size()];
                    }
                }

                auto neutral = levelIsDark ? Rgb{255, 255, 255} : Rgb{8, 8, 12};
                if (maximum) {
                    auto const opposite = levelIsDark ? Rgb{8, 8, 12} : Rgb{255, 255, 255};
                    if (visibilityScore(opposite) > visibilityScore(neutral)) neutral = opposite;
                }

                Palette palette;
                palette.colors = {
                    selected[0].color,
                    selected[1].color,
                    selected[2].color,
                    selected[1].color,
                    selected[2].color,
                    neutral,
                    selected[2].color,
                    selected[1].color,
                };
                return palette;
            };

            job->stage.store(3, std::memory_order_relaxed);
            job->progress.store(0.9f, std::memory_order_relaxed);

            auto const oppositeHue = std::fmod(dominantHue + 180.0, 360.0);
            AnalysisResult result;
            result.palettes[0] = buildPalette(levelIsDark ? 0.82 : 0.34, 0.20, oppositeHue, false);
            result.palettes[1] = buildPalette(levelIsDark ? 0.75 : 0.40, 0.13, std::fmod(oppositeHue + 70.0, 360.0), false);
            result.palettes[2] = buildPalette(levelIsDark ? 0.94 : 0.20, 0.25, std::fmod(oppositeHue + 290.0, 360.0), true);
            result.objectCount = objectCount;
            result.sampledColors = histogram.samples;

            job->progress.store(1.f, std::memory_order_relaxed);
            job->stage.store(4, std::memory_order_relaxed);
            return result;
        }

        uint64_t hashLevel(std::string_view data) {
            uint64_t hash = 14695981039346656037ull;
            for (auto const value : data) {
                hash ^= static_cast<uint8_t>(value);
                hash *= 1099511628211ull;
            }
            return hash;
        }

        std::string cacheKey(uint64_t hash) {
            return fmt::format("smart-contrast-cache-{:016x}", hash);
        }

        int64_t packColor(Rgb color) {
            return
                (static_cast<int64_t>(color.r) << 16) |
                (static_cast<int64_t>(color.g) << 8) |
                static_cast<int64_t>(color.b);
        }

        Rgb unpackColor(int64_t color) {
            return {
                static_cast<uint8_t>((color >> 16) & 0xff),
                static_cast<uint8_t>((color >> 8) & 0xff),
                static_cast<uint8_t>(color & 0xff),
            };
        }

        void saveCache(uint64_t hash, AnalysisResult const& result) {
            std::vector<int64_t> values;
            values.reserve(28);
            values.push_back(1);
            values.push_back(static_cast<int64_t>(result.objectCount));
            values.push_back(static_cast<int64_t>(result.sampledColors));
            for (auto const& palette : result.palettes) {
                for (auto const color : palette.colors) values.push_back(packColor(color));
            }
            Mod::get()->setSavedValue(cacheKey(hash), values);
        }

        std::optional<AnalysisResult> loadCache(uint64_t hash) {
            auto const key = cacheKey(hash);
            if (!Mod::get()->hasSavedValue(key)) return std::nullopt;
            auto const values = Mod::get()->getSavedValue<std::vector<int64_t>>(key);
            if (values.size() != 27 || values.front() != 1) return std::nullopt;

            AnalysisResult result;
            result.objectCount = static_cast<size_t>(std::max<int64_t>(values[1], 0));
            result.sampledColors = static_cast<size_t>(std::max<int64_t>(values[2], 0));
            size_t position = 3;
            for (auto& palette : result.palettes) {
                for (auto& color : palette.colors) color = unpackColor(values[position++]);
            }
            return result;
        }

        void applyPalette(Palette const& palette) {
            for (size_t index = 0; index < kColorSettingKeys.size(); ++index) {
                auto const color = palette.colors[index];
                Mod::get()->setSettingValue<ccColor4B>(
                    kColorSettingKeys[index],
                    {color.r, color.g, color.b, 255}
                );
            }
        }

        void addLevel(
            std::vector<LevelEntry>& output,
            std::unordered_set<std::string>& identities,
            GJGameLevel* level,
            bool editorLevel
        ) {
            if (!level || level->m_levelString.empty()) return;

            auto const id = static_cast<int>(level->m_levelID);
            auto name = std::string(level->m_levelName.c_str());
            if (name.empty()) name = "Unnamed level";

            auto identity = editorLevel
                ? fmt::format("editor:{}:{}", name, level->m_levelString.size())
                : id > 0
                    ? fmt::format("online:{}", id)
                    : fmt::format("saved:{}:{}", name, level->m_levelString.size());
            if (!identities.insert(identity).second) return;

            output.push_back({level, std::move(name), id, editorLevel});
        }

        std::vector<LevelEntry> collectLevels() {
            std::vector<LevelEntry> levels;
            std::unordered_set<std::string> identities;

            if (auto* local = LocalLevelManager::sharedState(); local && local->m_localLevels) {
                for (auto* level : geode::cocos::CCArrayExt<GJGameLevel*, false>(local->m_localLevels)) {
                    addLevel(levels, identities, level, true);
                }
            }

            if (auto* manager = GameLevelManager::sharedState(); manager && manager->m_storedLevels) {
                for (auto const& [key, level] : geode::cocos::CCDictionaryExt<std::string_view, GJGameLevel*, false>(manager->m_storedLevels)) {
                    (void)key;
                    addLevel(levels, identities, level, false);
                }
            }

            std::sort(levels.begin(), levels.end(), [](auto const& lhs, auto const& rhs) {
                auto first = lhs.name;
                auto second = rhs.name;
                std::transform(first.begin(), first.end(), first.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                std::transform(second.begin(), second.end(), second.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                return first < second;
            });
            return levels;
        }

        class PalettePopup;
        class AnalysisPopup;

        class PalettePopup final : public Popup {
        protected:
            AnalysisResult m_result;
            std::string m_levelName;
            std::string m_levelData;
            uint64_t m_hash = 0;

            static NineSlice* createSwatch(Rgb color, CCSize size) {
                auto* swatch = NineSlice::create("square02b_001.png");
                swatch->setColor({color.r, color.g, color.b});
                swatch->setOpacity(255);
                swatch->setContentSize(size);
                swatch->setAnchorPoint({.5f, .5f});
                return swatch;
            }

            bool init(std::string levelName, std::string levelData, uint64_t hash, AnalysisResult result, bool cached) {
                if (!Popup::init(450.f, 285.f)) return false;
                m_noElasticity = true;
                m_levelName = std::move(levelName);
                m_levelData = std::move(levelData);
                m_hash = hash;
                m_result = std::move(result);
                this->setTitle("Smart Contrast");

                auto* subtitle = CCLabelBMFont::create(
                    fmt::format("{}  -  {} objects{}", m_levelName, m_result.objectCount, cached ? "  (cached)" : "").c_str(),
                    "bigFont.fnt"
                );
                subtitle->setScale(.38f);
                subtitle->limitLabelWidth(390.f, .38f, .2f);
                m_mainLayer->addChildAtPosition(subtitle, Anchor::Top, ccp(0, -48));

                constexpr std::array<char const*, 3> names = {"Balanced", "Comfort", "Maximum"};
                constexpr std::array<char const*, 3> buttonTextures = {
                    "GJ_button_01.png", "GJ_button_05.png", "GJ_button_06.png"
                };

                for (size_t paletteIndex = 0; paletteIndex < m_result.palettes.size(); ++paletteIndex) {
                    auto* card = CCNode::create();
                    card->setContentSize({126.f, 155.f});
                    card->setAnchorPoint({.5f, .5f});
                    card->setPosition({96.f + 129.f * static_cast<float>(paletteIndex), 147.f});

                    auto* cardBackground = NineSlice::create("square02b_001.png");
                    cardBackground->setColor({0, 0, 0});
                    cardBackground->setOpacity(90);
                    cardBackground->setContentSize(card->getContentSize());
                    card->addChildAtPosition(cardBackground, Anchor::Center);

                    auto* name = CCLabelBMFont::create(names[paletteIndex], "goldFont.fnt");
                    name->setScale(.46f);
                    card->addChildAtPosition(name, Anchor::Top, ccp(0, -15));

                    for (size_t colorIndex = 0; colorIndex < 8; ++colorIndex) {
                        auto const column = static_cast<float>(colorIndex % 4);
                        auto const row = static_cast<float>(colorIndex / 4);
                        auto* swatch = createSwatch(m_result.palettes[paletteIndex].colors[colorIndex], {24.f, 26.f});
                        swatch->setPosition({22.f + column * 27.f, 96.f - row * 29.f});
                        card->addChild(swatch);
                    }

                    auto* applySprite = ButtonSprite::create(
                        "Apply", "bigFont.fnt", buttonTextures[paletteIndex], .62f
                    );
                    applySprite->setScale(.74f);
                    auto* applyButton = CCMenuItemExt::createSpriteExtra(
                        applySprite,
                        [this, paletteIndex](CCMenuItemSpriteExtra*) {
                            applyPalette(m_result.palettes[paletteIndex]);
                            Notification::create(
                                fmt::format("{} palette applied", paletteIndex == 0 ? "Balanced" : paletteIndex == 1 ? "Comfort" : "Maximum"),
                                NotificationIcon::Success
                            )->show();
                            this->onClose(nullptr);
                        }
                    );
                    auto* cardMenu = CCMenu::create();
                    cardMenu->setContentSize(card->getContentSize());
                    cardMenu->setAnchorPoint({0.f, 0.f});
                    cardMenu->setPosition({0.f, 0.f});
                    cardMenu->addChildAtPosition(applyButton, Anchor::Bottom, ccp(0, 14));
                    card->addChild(cardMenu, 2);
                    m_mainLayer->addChild(card);
                }

                auto* hint = CCLabelBMFont::create("8 swatches: hitboxes, player and trajectory", "bigFont.fnt");
                hint->setScale(.3f);
                hint->setOpacity(180);
                m_mainLayer->addChildAtPosition(hint, Anchor::Bottom, ccp(0, 26));

                auto* reanalyzeSprite = ButtonSprite::create("Reanalyze", "bigFont.fnt", "GJ_button_04.png", .55f);
                reanalyzeSprite->setScale(.65f);
                auto* reanalyze = CCMenuItemExt::createSpriteExtra(
                    reanalyzeSprite,
                    [this](CCMenuItemSpriteExtra*) {
                        auto name = m_levelName;
                        auto data = m_levelData;
                        auto hash = m_hash;
                        this->onClose(nullptr);
                        startAnalysis(std::move(name), std::move(data), hash);
                    }
                );
                m_buttonMenu->addChildAtPosition(reanalyze, Anchor::Bottom, ccp(0, 7));
                return true;
            }

        public:
            static PalettePopup* create(
                std::string levelName,
                std::string levelData,
                uint64_t hash,
                AnalysisResult result,
                bool cached
            ) {
                auto* popup = new PalettePopup();
                if (popup->init(std::move(levelName), std::move(levelData), hash, std::move(result), cached)) {
                    popup->autorelease();
                    return popup;
                }
                delete popup;
                return nullptr;
            }

            static void startAnalysis(std::string levelName, std::string levelData, uint64_t hash);
        };

        class AnalysisPopup final : public Popup {
        protected:
            std::shared_ptr<AnalysisJob> m_job;
            std::string m_levelName;
            std::string m_levelData;
            uint64_t m_hash = 0;
            NineSlice* m_progressFill = nullptr;
            CCLabelBMFont* m_progressLabel = nullptr;
            CCLabelBMFont* m_stageLabel = nullptr;
            bool m_delivered = false;
            std::jthread m_worker;

            ~AnalysisPopup() override {
                if (m_job && !m_job->complete.load(std::memory_order_acquire)) {
                    m_job->cancelled.store(true, std::memory_order_release);
                }
                if (m_worker.joinable()) m_worker.join();
            }

            bool init(std::string levelName, std::string levelData, uint64_t hash) {
                if (!Popup::init(380.f, 175.f)) return false;
                m_noElasticity = true;
                m_levelName = std::move(levelName);
                m_levelData = std::move(levelData);
                m_hash = hash;
                m_job = std::make_shared<AnalysisJob>();
                this->setTitle("Analyzing level");

                auto* levelLabel = CCLabelBMFont::create(m_levelName.c_str(), "goldFont.fnt");
                levelLabel->setScale(.5f);
                levelLabel->limitLabelWidth(320.f, .5f, .25f);
                m_mainLayer->addChildAtPosition(levelLabel, Anchor::Center, ccp(0, 37));

                m_stageLabel = CCLabelBMFont::create("Reading level data...", "bigFont.fnt");
                m_stageLabel->setScale(.38f);
                m_mainLayer->addChildAtPosition(m_stageLabel, Anchor::Center, ccp(0, 7));

                auto* progressBackground = NineSlice::create("square02b_001.png");
                progressBackground->setColor({0, 0, 0});
                progressBackground->setOpacity(120);
                progressBackground->setContentSize({300.f, 18.f});
                progressBackground->setAnchorPoint({.5f, .5f});
                m_mainLayer->addChildAtPosition(progressBackground, Anchor::Center, ccp(0, -25));

                m_progressFill = NineSlice::create("square02b_001.png");
                m_progressFill->setColor({60, 220, 120});
                m_progressFill->setOpacity(255);
                m_progressFill->setAnchorPoint({0.f, .5f});
                m_progressFill->setContentSize({1.f, 12.f});
                m_progressFill->setPosition({3.f, 9.f});
                progressBackground->addChild(m_progressFill);

                m_progressLabel = CCLabelBMFont::create("0%", "bigFont.fnt");
                m_progressLabel->setScale(.32f);
                m_mainLayer->addChildAtPosition(m_progressLabel, Anchor::Center, ccp(0, -49));

                auto job = m_job;
                auto data = m_levelData;
                m_worker = std::jthread([job, data = std::move(data)]() mutable {
                    auto result = analyzeLevel(data, job);
                    if (job->cancelled.load(std::memory_order_acquire)) return;
                    {
                        std::lock_guard lock(job->resultMutex);
                        job->result = std::move(result);
                    }
                    job->complete.store(true, std::memory_order_release);
                });

                this->schedule(schedule_selector(AnalysisPopup::updateProgress), 0.04f);
                return true;
            }

            void updateProgress(float) {
                auto const progress = std::clamp(m_job->progress.load(std::memory_order_relaxed), 0.f, 1.f);
                m_progressFill->setContentSize({std::max(1.f, 294.f * progress), 12.f});
                m_progressLabel->setString(fmt::format("{}%", static_cast<int>(std::lround(progress * 100.f))).c_str());

                constexpr std::array<char const*, 5> stages = {
                    "Reading level data...",
                    "Mapping colors and objects...",
                    "Scoring contrast...",
                    "Building three palettes...",
                    "Done",
                };
                auto const stage = std::clamp(m_job->stage.load(std::memory_order_relaxed), 0, 4);
                m_stageLabel->setString(stages[stage]);

                if (!m_delivered && m_job->complete.load(std::memory_order_acquire)) {
                    m_delivered = true;
                    std::optional<AnalysisResult> result;
                    {
                        std::lock_guard lock(m_job->resultMutex);
                        result = m_job->result;
                    }
                    if (!result) return;

                    saveCache(m_hash, *result);
                    auto levelName = m_levelName;
                    auto levelData = std::move(m_levelData);
                    auto hash = m_hash;
                    this->unschedule(schedule_selector(AnalysisPopup::updateProgress));
                    this->onClose(nullptr);
                    PalettePopup::create(std::move(levelName), std::move(levelData), hash, std::move(*result), false)->show();
                }
            }

        public:
            static AnalysisPopup* create(std::string levelName, std::string levelData, uint64_t hash) {
                auto* popup = new AnalysisPopup();
                if (popup->init(std::move(levelName), std::move(levelData), hash)) {
                    popup->autorelease();
                    return popup;
                }
                delete popup;
                return nullptr;
            }
        };

        void PalettePopup::startAnalysis(std::string levelName, std::string levelData, uint64_t hash) {
            AnalysisPopup::create(std::move(levelName), std::move(levelData), hash)->show();
        }

        class LevelPickerPopup final : public Popup {
        protected:
            std::vector<LevelEntry> m_levels;
            ScrollLayer* m_list = nullptr;
            TextInput* m_search = nullptr;
            CCLabelBMFont* m_emptyLabel = nullptr;

            bool init() override {
                if (!Popup::init(440.f, 285.f)) return false;
                m_noElasticity = true;
                m_levels = collectLevels();
                this->setTitle("Smart Contrast - Select Level");

                m_search = TextInput::create(346.f, "Search saved levels...");
                m_search->setTextAlign(TextInputAlign::Left);
                m_search->setCommonFilter(CommonFilter::Any);
                m_search->setMaxCharCount(80);
                m_search->setScale(.8f);
                m_search->setCallback([this](std::string const&) { this->rebuildList(); });
                m_mainLayer->addChildAtPosition(m_search, Anchor::Top, ccp(0, -48));

                auto* listBackground = CCLayerColor::create({0, 0, 0, 80});
                listBackground->setContentSize({370.f, 185.f});
                listBackground->ignoreAnchorPointForPosition(false);
                m_mainLayer->addChildAtPosition(listBackground, Anchor::Center, ccp(0, -22));

                m_list = ScrollLayer::create({360.f, 177.f});
                m_list->m_contentLayer->setLayout(ScrollLayer::createDefaultListLayout(4.f));
                listBackground->addChildAtPosition(m_list, Anchor::BottomLeft, ccp(5.f, 4.f));

                m_emptyLabel = CCLabelBMFont::create("No downloaded level data found", "bigFont.fnt");
                m_emptyLabel->setScale(.4f);
                m_emptyLabel->setOpacity(190);
                listBackground->addChildAtPosition(m_emptyLabel, Anchor::Center);

                this->rebuildList();
                return true;
            }

            void rebuildList() {
                if (!m_list) return;
                m_list->m_contentLayer->removeAllChildren();
                auto query = std::string(m_search ? m_search->getString().c_str() : "");
                std::transform(query.begin(), query.end(), query.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

                size_t visible = 0;
                for (size_t index = 0; index < m_levels.size(); ++index) {
                    auto searchName = m_levels[index].name;
                    std::transform(searchName.begin(), searchName.end(), searchName.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                    if (!query.empty() && searchName.find(query) == std::string::npos) continue;

                    auto const& level = m_levels[index];
                    auto caption = level.editorLevel
                        ? fmt::format("{}  [Editor]", level.name)
                        : level.id > 0
                            ? fmt::format("{}  [ID {}]", level.name, level.id)
                            : fmt::format("{}  [Saved]", level.name);
                    auto* sprite = ButtonSprite::create(caption.c_str(), 340, 0, .48f, true);
                    auto* button = CCMenuItemExt::createSpriteExtra(
                        sprite,
                        [this, index](CCMenuItemSpriteExtra*) { this->selectLevel(index); }
                    );
                    auto* row = CCMenu::create();
                    row->setContentSize({355.f, 34.f});
                    row->setAnchorPoint({.5f, .5f});
                    row->addChildAtPosition(button, Anchor::Center);
                    m_list->m_contentLayer->addChild(row);
                    ++visible;
                }

                m_emptyLabel->setVisible(visible == 0);
                m_list->m_contentLayer->updateLayout();
                m_list->scrollToTop();
            }

            void selectLevel(size_t index) {
                if (index >= m_levels.size()) return;
                auto const& entry = m_levels[index];
                if (!entry.level || entry.level->m_levelString.empty()) {
                    Notification::create("This level has no local data", NotificationIcon::Error)->show();
                    return;
                }

                // This is the only engine-owned read used by the analyzer. From
                // this point on, the worker sees a stable std::string copy only.
                auto data = std::string(entry.level->m_levelString.c_str());
                auto const hash = hashLevel(data);
                auto name = entry.name;

                if (auto cached = loadCache(hash)) {
                    PalettePopup::create(std::move(name), std::move(data), hash, std::move(*cached), true)->show();
                } else {
                    AnalysisPopup::create(std::move(name), std::move(data), hash)->show();
                }
            }

        public:
            static LevelPickerPopup* create() {
                auto* popup = new LevelPickerPopup();
                if (popup->init()) {
                    popup->autorelease();
                    return popup;
                }
                delete popup;
                return nullptr;
            }
        };
    }

    void openLevelPicker() {
        auto* popup = LevelPickerPopup::create();
        if (popup) popup->show();
    }
}

$on_mod(Loaded) {
    ButtonSettingPressedEventV3(Mod::get(), "smart-contrast").listen([](std::string_view buttonKey) {
        if (buttonKey == "analyze") cleanfeed::smart_contrast::openLevelPicker();
    }).leak();
}
