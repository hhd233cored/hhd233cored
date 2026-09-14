#include <array>
#include <cctype>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <new>
#include <string>
#include <unordered_map>
#include <vector>

namespace {
    using CodePoint = std::uint32_t;

    /*
    程序的整体处理流程：
    1. 从命令行参数中取得原文、抄袭版和答案文件路径。
    2. 读取两个输入文件，并将 UTF-8 字节解码为 Unicode 字符。
    3. 删除空白和常见标点，统一大小写和全角/半角形式。
    4. 将连续三个 Unicode 字符组成一个 n-gram 特征。
    5. 统计原文特征次数，并计算它与抄袭版特征的多重集合交集。
    6. 将交集数量除以抄袭版特征总数，得到最终重复率。
    */

    // 一个 feature 是由至多三个 Unicode 码点组成的序列。
    // 这种定长表示保证了键值的精确性
    // 在相似度计算过程中不涉及任何有损的 64 位特征哈希。
    struct Feature {
        std::array<CodePoint, 3> value{};
        std::uint8_t length = 0;

        // 判断两个特征的实际长度和每一个字符是否都相同。
        bool operator==(const Feature& other) const noexcept {
            return length == other.length && value == other.value;
        }
    };

    /*
    FeatureHash 是 unordered_map 使用的哈希函数。
    它只负责把 Feature 放入合适的哈希桶中，不直接参与重复率计算。
    unordered_map 仍会调用 Feature::operator== 比较完整的字符序列，
    因此即使两个不同特征产生相同哈希值，也不会被错误合并。
    */
    struct FeatureHash {
        // 为一个 Feature 生成哈希表内部使用的分桶编号。
        std::size_t operator()(const Feature& feature) const noexcept {
            // FNV-1a 仅用于将精确的 Feature 对象分配到哈希桶中。
            // 相等运算符（==）仍会对所有码点进行逐一比较
            // 因此哈希冲突不会影响计算结果。
            std::size_t hash = static_cast<std::size_t>(1469598103934665603ULL);
            hash ^= feature.length;
            hash *= static_cast<std::size_t>(1099511628211ULL);
            for (std::uint8_t i = 0; i < feature.length; ++i) {
                hash ^= static_cast<std::size_t>(feature.value[i]);
                hash *= static_cast<std::size_t>(1099511628211ULL);
            }
            return hash;
        }
    };

    // 保存每种 n-gram 在一篇文本中的出现次数。
    using FeatureFrequency = std::unordered_map<Feature, std::size_t, FeatureHash>;

    // 判断 byte 是否是 UTF-8 多字节字符的后续字节。
    // UTF-8 后续字节的二进制形式是 10xxxxxx。
    bool isContinuationByte(unsigned char byte) {
        return (byte & 0xC0U) == 0x80U;
    }

    /*
     * 以下几个函数负责 HTML 清洗。
     *
     * HTML 清洗发生在 UTF-8 解码和 n-gram 特征提取之前，结果只保存在
     * 内存中，不会生成额外的中间文件，因此正式评测时仍然只读两个输入
     * 文件，并向第三个参数指定的答案文件写入结果。
     */

    // 将只包含 ASCII 字母的部分转换为小写，用于不区分大小写地判断标签。
    std::string asciiLowerHtml(const std::string& text) {
        std::string result = text;
        for (char& character : result) {
            const unsigned char byte = static_cast<unsigned char>(character);
            if (byte >= static_cast<unsigned char>('A') &&
                byte <= static_cast<unsigned char>('Z')) {
                character = static_cast<char>(byte + ('a' - 'A'));
            }
        }
        return result;
    }

    // 判断一个 ASCII 字符是否为空白，用于扫描 HTML 标签本身。
    bool isAsciiSpaceHtml(char character) {
        return std::isspace(static_cast<unsigned char>(character)) != 0;
    }

    // 判断 HTML 原文中是否出现某个不区分大小写的标记。
    bool containsIgnoreCaseHtml(const std::string& text,
                                const std::string& pattern) {
        return asciiLowerHtml(text).find(asciiLowerHtml(pattern)) !=
               std::string::npos;
    }

    /*
     * 判断输入是否可能是 HTML。
     *
     * 只检查文件开头最多 8192 个字节，并跳过 BOM 和空白。普通文本中的
     * “a < b”不会仅因为出现一个小于号就被当作 HTML 处理。
     */
    bool looksLikeHtml(const std::string& text) {
        std::size_t start = 0;
        if (text.size() >= 3 &&
            static_cast<unsigned char>(text[0]) == 0xEFU &&
            static_cast<unsigned char>(text[1]) == 0xBBU &&
            static_cast<unsigned char>(text[2]) == 0xBFU) {
            start = 3;
        }
        while (start < text.size() && isAsciiSpaceHtml(text[start])) {
            ++start;
        }

        const std::size_t inspectLength =
            text.size() - start < 8192 ? text.size() - start : 8192;
        const std::string beginning =
            asciiLowerHtml(text.substr(start, inspectLength));
        return beginning.find("<!doctype html") != std::string::npos ||
               beginning.find("<html") != std::string::npos;
    }

    /*
     * 查找 HTML 标签结束位置。
     * 属性值中可能包含“>”，所以只有在不处于单双引号中时才结束标签。
     */
    std::size_t findHtmlTagEnd(const std::string& text, std::size_t start) {
        char quote = '\0';
        for (std::size_t index = start; index < text.size(); ++index) {
            const char character = text[index];
            if (quote != '\0') {
                if (character == quote) {
                    quote = '\0';
                }
            } else if (character == '\'' || character == '"') {
                quote = character;
            } else if (character == '>') {
                return index;
            }
        }
        return std::string::npos;
    }

    // 保存一个标签的基本信息，供清洗状态机使用。
    struct HtmlTagInfo {
        bool valid = false;
        bool closing = false;
        bool selfClosing = false;
        std::string name;
        std::string lowerText;
    };

    /*
     * 解析一个完整标签，例如：
     *   <td class="blob-code">：name=td，closing=false
     *   </td>                 ：name=td，closing=true
     */
    HtmlTagInfo parseHtmlTag(const std::string& tag) {
        HtmlTagInfo result;
        result.lowerText = asciiLowerHtml(tag);
        if (tag.size() < 3 || tag.front() != '<' || tag.back() != '>') {
            return result;
        }

        std::size_t index = 1;
        while (index + 1 < tag.size() && isAsciiSpaceHtml(tag[index])) {
            ++index;
        }
        if (index + 1 >= tag.size() || tag[index] == '!' || tag[index] == '?') {
            // DOCTYPE、注释和处理指令不作为普通标签解析。
            return result;
        }

        if (tag[index] == '/') {
            result.closing = true;
            ++index;
            while (index + 1 < tag.size() && isAsciiSpaceHtml(tag[index])) {
                ++index;
            }
        }

        const std::size_t nameStart = index;
        while (index + 1 < tag.size() &&
               !isAsciiSpaceHtml(tag[index]) && tag[index] != '/' &&
               tag[index] != '>') {
            ++index;
        }
        if (index == nameStart) {
            return result;
        }

        result.name = asciiLowerHtml(tag.substr(nameStart, index - nameStart));
        result.valid = true;

        std::size_t beforeEnd = tag.size() - 1;
        while (beforeEnd > 0 && isAsciiSpaceHtml(tag[beforeEnd - 1])) {
            --beforeEnd;
        }
        result.selfClosing = beforeEnd > 0 && tag[beforeEnd - 1] == '/';
        return result;
    }

    // 在 script/style 等原始文本元素中查找对应的结束标签。
    std::size_t findHtmlClosingTag(const std::string& text,
                                   std::size_t start,
                                   const std::string& tagName) {
        const std::string marker = "</" + asciiLowerHtml(tagName);
        const std::string lowerText = asciiLowerHtml(text);
        std::size_t position = lowerText.find(marker, start);
        while (position != std::string::npos) {
            const std::size_t afterName = position + marker.size();
            if (afterName >= text.size() || isAsciiSpaceHtml(text[afterName]) ||
                text[afterName] == '>') {
                return position;
            }
            position = lowerText.find(marker, position + 1);
        }
        return std::string::npos;
    }

    // 将 HTML 数字实体解码后重新编码为 UTF-8。
    void appendUtf8CodePoint(CodePoint codePoint, std::string& output) {
        if (codePoint <= 0x7FU) {
            output.push_back(static_cast<char>(codePoint));
        } else if (codePoint <= 0x7FFU) {
            output.push_back(static_cast<char>(0xC0U | (codePoint >> 6U)));
            output.push_back(static_cast<char>(0x80U | (codePoint & 0x3FU)));
        } else if (codePoint <= 0xFFFFU) {
            output.push_back(static_cast<char>(0xE0U | (codePoint >> 12U)));
            output.push_back(static_cast<char>(0x80U | ((codePoint >> 6U) & 0x3FU)));
            output.push_back(static_cast<char>(0x80U | (codePoint & 0x3FU)));
        } else if (codePoint <= 0x10FFFFU) {
            output.push_back(static_cast<char>(0xF0U | (codePoint >> 18U)));
            output.push_back(static_cast<char>(0x80U | ((codePoint >> 12U) & 0x3FU)));
            output.push_back(static_cast<char>(0x80U | ((codePoint >> 6U) & 0x3FU)));
            output.push_back(static_cast<char>(0x80U | (codePoint & 0x3FU)));
        }
    }

    /*
     * 解码正文中常见的 HTML 实体，例如 &lt;、&gt;、&amp;、&quot; 和数字实体。
     * 未知实体保持原样，避免清洗过程中丢失正文信息。
     */
    std::string decodeHtmlEntities(const std::string& text) {
        std::string output;
        output.reserve(text.size());

        for (std::size_t index = 0; index < text.size();) {
            if (text[index] != '&') {
                output.push_back(text[index++]);
                continue;
            }

            const std::size_t semicolon = text.find(';', index + 1);
            if (semicolon == std::string::npos || semicolon - index > 16) {
                output.push_back(text[index++]);
                continue;
            }

            const std::string entity = text.substr(index + 1, semicolon - index - 1);
            const std::string lowerEntity = asciiLowerHtml(entity);
            if (lowerEntity == "amp") {
                output.push_back('&');
            } else if (lowerEntity == "lt") {
                output.push_back('<');
            } else if (lowerEntity == "gt") {
                output.push_back('>');
            } else if (lowerEntity == "quot") {
                output.push_back('"');
            } else if (lowerEntity == "apos") {
                output.push_back('\'');
            } else if (lowerEntity == "nbsp") {
                output.push_back(' ');
            } else if (!entity.empty() && entity[0] == '#') {
                int base = 10;
                std::size_t digitStart = 1;
                if (digitStart < entity.size() &&
                    (entity[digitStart] == 'x' || entity[digitStart] == 'X')) {
                    base = 16;
                    ++digitStart;
                }

                CodePoint value = 0;
                bool valid = digitStart < entity.size();
                for (std::size_t digit = digitStart; valid && digit < entity.size(); ++digit) {
                    const unsigned char character =
                        static_cast<unsigned char>(entity[digit]);
                    int digitValue = -1;
                    if (character >= '0' && character <= '9') {
                        digitValue = character - '0';
                    } else if (base == 16 && character >= 'a' && character <= 'f') {
                        digitValue = character - 'a' + 10;
                    } else if (base == 16 && character >= 'A' && character <= 'F') {
                        digitValue = character - 'A' + 10;
                    }
                    if (digitValue < 0 || digitValue >= base ||
                        value > (0x10FFFFU - static_cast<CodePoint>(digitValue)) /
                                    static_cast<CodePoint>(base)) {
                        valid = false;
                        break;
                    }
                    value = value * static_cast<CodePoint>(base) +
                            static_cast<CodePoint>(digitValue);
                }

                if (valid && value <= 0x10FFFFU &&
                    !(value >= 0xD800U && value <= 0xDFFFU)) {
                    appendUtf8CodePoint(value, output);
                } else {
                    output.append(text, index, semicolon - index + 1);
                }
            } else {
                output.append(text, index, semicolon - index + 1);
            }
            index = semicolon + 1;
        }
        return output;
    }

    bool isHtmlBlockTag(const std::string& tagName) {
        return tagName == "p" || tagName == "div" || tagName == "section" ||
               tagName == "article" || tagName == "li" || tagName == "tr" ||
               tagName == "h1" || tagName == "h2" || tagName == "h3" ||
               tagName == "h4" || tagName == "h5" || tagName == "h6";
    }

    /*
     * 清洗一份已经确认是 HTML 的文件。
     *
     * 当前测试文件来自 GitHub 网页，真正的原文位于 class 属性包含
     * “blob-code”的 <td> 中。因此检测到 blob-code 时只保留这些单元格；
     * 其他 HTML 则保留普通可见文字，并跳过非正文区域。
     */
    std::string cleanHtmlDocument(const std::string& html) {
        const bool onlyBlobCode = containsIgnoreCaseHtml(html, "blob-code");
        std::string output;
        output.reserve(html.size());

        bool inBlobCode = false;
        std::string skippedTag;

        for (std::size_t index = 0; index < html.size();) {
            if (!skippedTag.empty()) {
                const std::size_t closing =
                    findHtmlClosingTag(html, index, skippedTag);
                if (closing == std::string::npos) {
                    break;
                }
                const std::size_t closingEnd = findHtmlTagEnd(html, closing);
                if (closingEnd == std::string::npos) {
                    break;
                }
                skippedTag.clear();
                index = closingEnd + 1;
                continue;
            }

            if (html.compare(index, 4, "<!--") == 0) {
                const std::size_t commentEnd = html.find("-->", index + 4);
                index = commentEnd == std::string::npos ? html.size() : commentEnd + 3;
                continue;
            }

            if (html[index] != '<') {
                const std::size_t nextTag = html.find('<', index);
                const std::size_t textEnd =
                    nextTag == std::string::npos ? html.size() : nextTag;
                if (!onlyBlobCode || inBlobCode) {
                    output += decodeHtmlEntities(html.substr(index, textEnd - index));
                }
                index = textEnd;
                continue;
            }

            const std::size_t tagEnd = findHtmlTagEnd(html, index);
            if (tagEnd == std::string::npos) {
                if (!onlyBlobCode || inBlobCode) {
                    output.push_back(html[index]);
                }
                ++index;
                continue;
            }

            const HtmlTagInfo tag =
                parseHtmlTag(html.substr(index, tagEnd - index + 1));
            if (tag.valid) {
                if (!tag.closing && !tag.selfClosing &&
                    (tag.name == "script" || tag.name == "style" ||
                     tag.name == "head" || tag.name == "nav" ||
                     tag.name == "header" || tag.name == "footer" ||
                     tag.name == "form" || tag.name == "svg" ||
                     tag.name == "noscript" || tag.name == "template")) {
                    skippedTag = tag.name;
                } else if (onlyBlobCode) {
                    if (!tag.closing && tag.name == "td" &&
                        tag.lowerText.find("blob-code") != std::string::npos) {
                        inBlobCode = true;
                    } else if (tag.closing && tag.name == "td" && inBlobCode) {
                        inBlobCode = false;
                        output.push_back('\n');
                    }
                } else if (tag.name == "br" || isHtmlBlockTag(tag.name)) {
                    output.push_back('\n');
                }
            }
            index = tagEnd + 1;
        }
        return output;
    }

    /*
    将 UTF-8 字节字符串转换为 Unicode 码点数组。
     
    参数 text 是从输入文件中读取的原始字节内容，返回值中的每个元素
    表示一个 Unicode 字符。中文字符通常会被解码成三个字节对应的
    一个码点，而不是被错误地当成三个独立字符。
     
    函数会跳过 UTF-8 文件头 BOM。如果遇到非法 UTF-8 起始字节、缺失
    后续字节、过长编码或 UTF-16 代理区码点，就使用 U+FFFD 代替，
    然后继续处理，避免畸形输入导致程序异常退出。
    */
    std::vector<CodePoint> decodeUtf8(const std::string& text) {
        std::vector<CodePoint> codePoints;
        codePoints.reserve(text.size());

        std::size_t index = 0;
        if (text.size() >= 3 &&
            static_cast<unsigned char>(text[0]) == 0xEFU &&
            static_cast<unsigned char>(text[1]) == 0xBBU &&
            static_cast<unsigned char>(text[2]) == 0xBFU) {
            index = 3; // 跳过 3 字节的 UTF-8 字节顺序标记（BOM 头）
        }

        while (index < text.size()) {
            const unsigned char first = static_cast<unsigned char>(text[index]);

            if (first <= 0x7FU) {
                // ASCII 字符本身就是一个完整的 UTF-8 字符。
                codePoints.push_back(first);
                ++index;
                continue;
            }

            std::size_t length = 0;
            CodePoint codePoint = 0;
            CodePoint minimumValue = 0;

            if ((first & 0xE0U) == 0xC0U) {
                // 110xxxxx 10xxxxxx：两个字节。
                length = 2;
                codePoint = first & 0x1FU;
                minimumValue = 0x80U;
            }
            else if ((first & 0xF0U) == 0xE0U) {
                // 1110xxxx 10xxxxxx 10xxxxxx：三个字节，中文通常属于此类。
                length = 3;
                codePoint = first & 0x0FU;
                minimumValue = 0x800U;
            }
            else if ((first & 0xF8U) == 0xF0U) {
                // 11110xxx 10xxxxxx 10xxxxxx 10xxxxxx：四个字节。
                length = 4;
                codePoint = first & 0x07U;
                minimumValue = 0x10000U;
            }
            else {
                // 当前字节不是合法的 UTF-8 起始字节。
                codePoints.push_back(0xFFFDU);
                ++index;
                continue;
            }

            if (index + length > text.size()) {
                // 当前字符的后续字节不完整。
                codePoints.push_back(0xFFFDU);
                ++index;
                continue;
            }

            bool valid = true;
            for (std::size_t offset = 1; offset < length; ++offset) {
                if (!isContinuationByte(static_cast<unsigned char>(text[index + offset]))) {
                    valid = false;
                    break;
                }
                codePoint = (codePoint << 6U) |
                    (static_cast<unsigned char>(text[index + offset]) & 0x3FU);
            }

            if (!valid || codePoint < minimumValue || codePoint > 0x10FFFFU ||
                (codePoint >= 0xD800U && codePoint <= 0xDFFFU)) {
                // 拒绝非法编码、过长编码以及 UTF-16 代理区码点。
                codePoints.push_back(0xFFFDU);
                ++index;
                continue;
            }

            codePoints.push_back(codePoint);
            index += length;
        }

        return codePoints;
    }

    /*
    归一化一个 Unicode 码点。
     
    这里统一三类容易造成误判的写法：
    1. 全角数字转换为半角数字；
    2. 全角英文字母转换为半角字母；
    3. 英文大写字母转换为小写。
     
    例如，全角“Ａ”和普通“a”最终都会以相同的字符参与匹配。
    */
    CodePoint normalizeCodePoint(CodePoint codePoint) {
        // 归一化中文文档中常见的全角 ASCII 字符（将其转换为普通半角字符）。
        if (codePoint == 0x3000U) {
            return 0x20U;
        }
        if (codePoint >= 0xFF10U && codePoint <= 0xFF19U) {
            return codePoint - 0xFEE0U;
        }
        if (codePoint >= 0xFF21U && codePoint <= 0xFF3AU) {
            return codePoint - 0xFEE0U;
        }
        if (codePoint >= 0xFF41U && codePoint <= 0xFF5AU) {
            return codePoint - 0xFEE0U;
        }
        if (codePoint >= static_cast<CodePoint>('A') &&
            codePoint <= static_cast<CodePoint>('Z')) {
            return codePoint + static_cast<CodePoint>('a' - 'A');
        }
        return codePoint;
    }

    /*
    判断一个 Unicode 码点是否为空白字符。
    除 ASCII 空格、换行和制表符外，还处理中文全角空格以及常见的
    Unicode 空格，避免排版差异影响重复率。
    */
    bool isWhitespace(CodePoint codePoint) {
        if (codePoint <= 0x20U || codePoint == 0x00A0U || codePoint == 0x1680U ||
            codePoint == 0x2028U || codePoint == 0x2029U || codePoint == 0x202FU ||
            codePoint == 0x205FU || codePoint == 0x3000U) {
            return true;
        }
        return codePoint >= 0x2000U && codePoint <= 0x200AU;
    }

    /*
    判断一个 Unicode 码点是否为标点符号。
    std::ispunct 只能可靠处理 ASCII 字符，所以对中文常见标点所在的
    Unicode 区间进行额外判断，例如“，”“。” “！” “（）”等。
    */
    bool isPunctuation(CodePoint codePoint) {
        if (codePoint < 0x80U) {
            return std::ispunct(static_cast<unsigned char>(codePoint)) != 0;
        }

        // 中英文字符常用的 Unicode 标点符号块。
        return (codePoint >= 0x2000U && codePoint <= 0x206FU) ||
            (codePoint >= 0x2E00U && codePoint <= 0x2E7FU) ||
            (codePoint >= 0x3000U && codePoint <= 0x303FU) ||
            (codePoint >= 0xFE10U && codePoint <= 0xFE1FU) ||
            (codePoint >= 0xFE30U && codePoint <= 0xFE4FU) ||
            (codePoint >= 0xFF01U && codePoint <= 0xFF65U);
    }

    /*
    预处理整篇文本，返回可以直接用于特征生成的 Unicode 字符序列。
     
    处理顺序为：
    1. UTF-8 解码；
    2. 统一大小写和全角/半角形式；
    3. 删除空白和标点；
    4. 保留剩余字符。
     
    本轻量版不依赖 jieba，而是使用字符级特征。这样可以避免额外的
    分词库和停用词文件，也能直接在评测机上用标准库编译。
    */
    std::vector<CodePoint> normalizeText(const std::string& text) {
        const std::vector<CodePoint> decoded = decodeUtf8(text);
        std::vector<CodePoint> normalized;
        normalized.reserve(decoded.size());

        for (CodePoint codePoint : decoded) {
            codePoint = normalizeCodePoint(codePoint);
            if (isWhitespace(codePoint) || isPunctuation(codePoint)) {
                continue;
            }
            normalized.push_back(codePoint);
        }

        return normalized;
    }

    /*
     * 准备一份输入文本供查重使用。
     *
     * 如果检测到 HTML，先在内存中提取正文并删除网页代码；如果是普通
     * 文本，则直接使用原内容。归一化完成后立即释放原始字符串和清洗
     * 字符串，降低正式评测时的内存峰值。
     */
    std::vector<CodePoint> prepareInputText(std::string& content) {
        if (!looksLikeHtml(content)) {
            std::vector<CodePoint> result = normalizeText(content);
            content.clear();
            content.shrink_to_fit();
            return result;
        }

        std::string cleanedHtml = cleanHtmlDocument(content);
        content.clear();
        content.shrink_to_fit();

        std::vector<CodePoint> result = normalizeText(cleanedHtml);
        cleanedHtml.clear();
        cleanedHtml.shrink_to_fit();
        return result;
    }

    /*
    从 text 的 start 位置取出 length 个字符，构造一个 n-gram 特征。
    默认 length 为 3；当文本很短时，length 也可能是 1 或 2。
    */
    Feature makeFeature(const std::vector<CodePoint>& text,
        std::size_t start,
        std::size_t length) {
        Feature feature;
        feature.length = static_cast<std::uint8_t>(length);
        for (std::size_t offset = 0; offset < length; ++offset) {
            feature.value[offset] = text[start + offset];
        }
        return feature;
    }

    /*
    统计一篇文本中所有 n-gram 的出现次数。
     
    例如 text 为“abcdef”、featureLength 为 3 时，生成：
      abc、bcd、cde、def
     
    返回的哈希表中，key 是具体的字符序列，value 是该序列的出现次数。
    */
    FeatureFrequency buildFrequency(const std::vector<CodePoint>& text,
        std::size_t featureLength) {
        FeatureFrequency frequency;
        if (featureLength == 0 || text.size() < featureLength) {
            return frequency;
        }

        const std::size_t featureCount = text.size() - featureLength + 1;
        frequency.reserve(featureCount);
        for (std::size_t index = 0; index < featureCount; ++index) {
            // operator[] 会在特征第一次出现时创建计数，并将计数加一。
            ++frequency[makeFeature(text, index, featureLength)];
        }
        return frequency;
    }

    /*
    选择 n-gram 的长度。
     
    正常情况下使用 3-gram。如果两篇文本中较短的一篇不足 3 个字符，
    则改用长度 1 或 2，保证短文本仍然可以计算：
      - 较短文本至少 3 个字符：使用 3；
      - 较短文本有 2 个字符：使用 2；
      - 较短文本有 1 个字符：使用 1；
      - 空文本：返回 0，最终重复率为 0。
    */
    std::size_t chooseFeatureLength(std::size_t originalSize,
        std::size_t plagiarizedSize) {
        const std::size_t shorterSize =
            originalSize < plagiarizedSize ? originalSize : plagiarizedSize;
        if (shorterSize >= 3) {
            return 3;
        }
        return shorterSize;
    }

    /*
    计算最终重复率。
    
    公式：
    matched = Σ min(原文中某特征的次数, 抄袭版中某特征的次数)
    rate    = matched / 抄袭版特征总数
    
    为了减少内存占用，函数只建立原文的频率表。扫描抄袭版时，每匹配
    到一个特征，就将原文中该特征的剩余次数减一，得到两个多重集合的
    交集大小，而不需要再建立抄袭版的第二张频率表。
    
    在哈希表平均性能下，时间复杂度为 O(n + m)，其中 n、m 分别是两篇
    文本的有效字符数；额外空间主要用于原文特征表。
    */
    double calculateRepeatRate(const std::vector<CodePoint>& original,
        const std::vector<CodePoint>& plagiarized) {
        const std::size_t featureLength =
            chooseFeatureLength(original.size(), plagiarized.size());
        if (featureLength == 0 || plagiarized.size() < featureLength) {
            return 0.0;
        }

        FeatureFrequency originalFrequency =
            buildFrequency(original, featureLength);
        const std::size_t plagiarizedFeatureCount =
            plagiarized.size() - featureLength + 1;

        // 扫描抄袭版时消耗原文中可匹配的次数，得到多重集合交集。
        std::size_t matchedFeatureCount = 0;
        for (std::size_t index = 0; index < plagiarizedFeatureCount; ++index) {
            const Feature feature = makeFeature(plagiarized, index, featureLength);
            const auto iterator = originalFrequency.find(feature);
            if (iterator != originalFrequency.end() && iterator->second > 0) {
                --iterator->second;
                ++matchedFeatureCount;
            }
        }

        if (plagiarizedFeatureCount == 0) {
            return 0.0;
        }

        const double rate = static_cast<double>(matchedFeatureCount) /
            static_cast<double>(plagiarizedFeatureCount);
        return rate < 0.0 ? 0.0 : (rate > 1.0 ? 1.0 : rate);
    }

    /*
    读取指定路径的整个文件。
    
    使用二进制模式是为了保留原始 UTF-8 字节，并让 decodeUtf8 统一处理
    换行。函数只访问传入的 path，不会扫描目录或读取其他文件。
    
    返回 true 表示读取成功；返回 false 表示文件不存在、无法获取文件
    大小、文件过大或读取过程中发生错误。
     */
    bool readFile(const char* path, std::string& content) {
        std::ifstream input(path, std::ios::binary);
        if (!input) {
            return false;
        }

        input.seekg(0, std::ios::end);
        const std::streampos endPosition = input.tellg();
        if (endPosition < 0) {
            return false;
        }
        input.seekg(0, std::ios::beg);

        const auto size = static_cast<std::uintmax_t>(endPosition);
        if (size > static_cast<std::uintmax_t>(
            std::numeric_limits<std::size_t>::max()) ||
            size > static_cast<std::uintmax_t>(
                std::numeric_limits<std::streamsize>::max())) {
            return false;
        }

        content.resize(static_cast<std::size_t>(size));
        if (!content.empty()) {
            input.read(&content[0], static_cast<std::streamsize>(content.size()));
            if (!input) {
                return false;
            }
        }
        return true;
    }
} // namespace

/*
评测时要求传入三个参数：
  argv[1]：原文文件路径
  argv[2]：抄袭版论文文件路径
  argv[3]：答案文件路径
 
成功时向答案文件写入一个固定两位小数的重复率，并返回 0。
参数错误、输入文件读取失败、输出文件写入失败或内存不足时返回非零
状态码，同时通过标准错误流给出提示。
 */
int main(int argc, char* argv[]) {
    if (argc != 4) {
        std::cerr << "Usage: main.exe <original> <plagiarized> <answer>\n";
        return 1;
    }

    try {
        std::string originalContent;
        std::string plagiarizedContent;

        // 在打开输出文件前先读取两个输入文件。
        // 这样即使答案路径与某个输入路径相同，也不会提前清空输入文件。
        if (!readFile(argv[1], originalContent)) {
            std::cerr << "Cannot read original file.\n";
            return 2;
        }
        if (!readFile(argv[2], plagiarizedContent)) {
            std::cerr << "Cannot read plagiarized file.\n";
            return 2;
        }

        // HTML 输入会先在内存中提取正文；普通文本则保持原来的处理方式。
        const std::vector<CodePoint> original = prepareInputText(originalContent);
        const std::vector<CodePoint> plagiarized =
            prepareInputText(plagiarizedContent);

        // 此时原始字符串已经释放，calculateRepeatRate 只处理归一化后的数据。
        const double repeatRate = calculateRepeatRate(original, plagiarized);

        // 每次运行都覆盖旧答案，且只写入数字和一个换行符。
        std::ofstream answer(argv[3], std::ios::binary | std::ios::trunc);
        if (!answer) {
            std::cerr << "Cannot write answer file.\n";
            return 3;
        }

        answer << std::fixed << std::setprecision(2) << repeatRate << '\n';
        if (!answer) {
            std::cerr << "Cannot finish writing answer file.\n";
            return 3;
        }
    } catch (const std::bad_alloc&) {
        // 大文件导致内存不足时，返回错误码而不是让程序异常终止。
        std::cerr << "Not enough memory.\n";
        return 4;
    } catch (const std::exception& exception) {
        // 处理标准库可能抛出的其他可识别异常。
        std::cerr << "Processing failed: " << exception.what() << '\n';
        return 4;
    } catch (...) {
        // 最后的保护分支，避免未知异常造成异常退出。
        std::cerr << "Processing failed.\n";
        return 4;
    }

    return 0;
}
