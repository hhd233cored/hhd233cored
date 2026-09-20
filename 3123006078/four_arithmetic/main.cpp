#include <algorithm>
#include <cctype>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <random>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace {

// 去掉字符串两端的空白字符，用于处理命令行参数、文件行和表达式片段。
std::string trim(const std::string& value) {
    std::size_t first = 0;
    while (first < value.size() && std::isspace(static_cast<unsigned char>(value[first]))) {
        ++first;
    }

    std::size_t last = value.size();
    while (last > first && std::isspace(static_cast<unsigned char>(value[last - 1]))) {
        --last;
    }
    return value.substr(first, last - first);
}

// 将字符串中所有出现的源字符串替换成目标字符串。
std::string replaceAll(std::string value, const std::string& from, const std::string& to) {
    std::size_t position = 0;
    while ((position = value.find(from, position)) != std::string::npos) {
        value.replace(position, from.size(), to);
        position += to.size();
    }
    return value;
}

// 将题目中可能出现的 Unicode 运算符转换成解析器内部使用的 ASCII 符号。
// 例如：−、×、÷、’ 分别转换成 -、*、/、'。
std::string normalizeSymbols(std::string value) {
    value = replaceAll(std::move(value), u8"−", "-");
    value = replaceAll(std::move(value), u8"×", "*");
    value = replaceAll(std::move(value), u8"÷", "/");
    value = replaceAll(std::move(value), u8"’", "'");
    return value;
}

std::uint64_t absoluteValue(std::int64_t value) {
    if (value >= 0) {
        return static_cast<std::uint64_t>(value);
    }
    // 不能直接对 INT64_MIN 做取负运算，这种写法也能正确处理 INT64_MIN。
    return static_cast<std::uint64_t>(-(value + 1)) + 1;
}

// 求两个无符号整数的最大公约数，用于约分分数。
std::uint64_t gcd64(std::uint64_t left, std::uint64_t right) {
    while (right != 0) {
        const std::uint64_t remainder = left % right;
        left = right;
        right = remainder;
    }
    return left == 0 ? 1 : left;
}

// 以下三个函数在进行整数运算前检查溢出，避免生成大范围分数时发生未定义行为。
bool checkedMultiply(std::int64_t left, std::int64_t right, std::int64_t& result) {
    if (left == 0 || right == 0) {
        result = 0;
        return true;
    }
    if ((left == -1 && right == std::numeric_limits<std::int64_t>::min()) ||
        (right == -1 && left == std::numeric_limits<std::int64_t>::min())) {
        return false;
    }
    if (left > 0) {
        if (right > 0 && left > std::numeric_limits<std::int64_t>::max() / right) {
            return false;
        }
        if (right < 0 && right < std::numeric_limits<std::int64_t>::min() / left) {
            return false;
        }
    } else {
        if (right > 0 && left < std::numeric_limits<std::int64_t>::min() / right) {
            return false;
        }
        if (right < 0 && left < std::numeric_limits<std::int64_t>::max() / right) {
            return false;
        }
    }
    result = left * right;
    return true;
}

bool checkedAdd(std::int64_t left, std::int64_t right, std::int64_t& result) {
    if ((right > 0 && left > std::numeric_limits<std::int64_t>::max() - right) ||
        (right < 0 && left < std::numeric_limits<std::int64_t>::min() - right)) {
        return false;
    }
    result = left + right;
    return true;
}

bool checkedSubtract(std::int64_t left, std::int64_t right, std::int64_t& result) {
    if ((right < 0 && left > std::numeric_limits<std::int64_t>::max() + right) ||
        (right > 0 && left < std::numeric_limits<std::int64_t>::min() + right)) {
        return false;
    }
    result = left - right;
    return true;
}

class Fraction {
public:
    // 分数始终保存为“已经约分、分母为正”的形式。
    std::int64_t numerator = 0;
    std::int64_t denominator = 1;

    Fraction() = default;

    Fraction(std::int64_t numeratorValue, std::int64_t denominatorValue)
        : numerator(numeratorValue), denominator(denominatorValue) {
        if (denominator == 0) {
            throw std::invalid_argument("分母不能为 0");
        }
        if (denominator < 0) {
            numerator = -numerator;
            denominator = -denominator;
        }
        // 构造时立即约分，保证 1/2 和 2/4 在比较时被视为同一个分数。
        const std::int64_t divisor = static_cast<std::int64_t>(
            gcd64(absoluteValue(numerator), absoluteValue(denominator)));
        numerator /= divisor;
        denominator /= divisor;
    }

    static bool fromRaw(std::int64_t numeratorValue,
                        std::int64_t denominatorValue,
                        Fraction& result) {
        if (denominatorValue == 0) {
            return false;
        }
        if (denominatorValue < 0) {
            numeratorValue = -numeratorValue;
            denominatorValue = -denominatorValue;
        }

        result = Fraction(numeratorValue, denominatorValue);
        return true;
    }

    // 加法：先通分，再用安全整数运算计算分子和分母。
    static bool add(const Fraction& left, const Fraction& right, Fraction& result) {
        const std::int64_t commonDivisor = static_cast<std::int64_t>(
            gcd64(static_cast<std::uint64_t>(left.denominator),
                  static_cast<std::uint64_t>(right.denominator)));
        const std::int64_t leftDenominator = left.denominator / commonDivisor;
        const std::int64_t rightDenominator = right.denominator / commonDivisor;
        std::int64_t leftNumerator = 0;
        std::int64_t rightNumerator = 0;
        std::int64_t numerator = 0;
        std::int64_t denominator = 0;
        return checkedMultiply(left.numerator, rightDenominator, leftNumerator) &&
               checkedMultiply(right.numerator, leftDenominator, rightNumerator) &&
               checkedAdd(leftNumerator, rightNumerator, numerator) &&
               checkedMultiply(left.denominator, rightDenominator, denominator) &&
               fromRaw(numerator, denominator, result);
    }

    // 减法由调用方保证结果不为负；这里负责通分和溢出检查。
    static bool subtract(const Fraction& left, const Fraction& right, Fraction& result) {
        const std::int64_t commonDivisor = static_cast<std::int64_t>(
            gcd64(static_cast<std::uint64_t>(left.denominator),
                  static_cast<std::uint64_t>(right.denominator)));
        const std::int64_t leftDenominator = left.denominator / commonDivisor;
        const std::int64_t rightDenominator = right.denominator / commonDivisor;
        std::int64_t leftNumerator = 0;
        std::int64_t rightNumerator = 0;
        std::int64_t numerator = 0;
        std::int64_t denominator = 0;
        return checkedMultiply(left.numerator, rightDenominator, leftNumerator) &&
               checkedMultiply(right.numerator, leftDenominator, rightNumerator) &&
               checkedSubtract(leftNumerator, rightNumerator, numerator) &&
               checkedMultiply(left.denominator, rightDenominator, denominator) &&
               fromRaw(numerator, denominator, result);
    }

    // 乘法先约去交叉公因数，可以显著降低中间结果溢出的概率。
    static bool multiply(const Fraction& left, const Fraction& right, Fraction& result) {
        std::int64_t leftNumerator = left.numerator;
        std::int64_t rightNumerator = right.numerator;
        std::int64_t leftDenominator = left.denominator;
        std::int64_t rightDenominator = right.denominator;
        const std::int64_t firstDivisor = static_cast<std::int64_t>(
            gcd64(absoluteValue(leftNumerator), static_cast<std::uint64_t>(rightDenominator)));
        leftNumerator /= firstDivisor;
        rightDenominator /= firstDivisor;
        const std::int64_t secondDivisor = static_cast<std::int64_t>(
            gcd64(absoluteValue(rightNumerator), static_cast<std::uint64_t>(leftDenominator)));
        rightNumerator /= secondDivisor;
        leftDenominator /= secondDivisor;
        std::int64_t numerator = 0;
        std::int64_t denominator = 0;
        return checkedMultiply(leftNumerator, rightNumerator, numerator) &&
               checkedMultiply(leftDenominator, rightDenominator, denominator) &&
               fromRaw(numerator, denominator, result);
    }

    // 除法等价于乘以右侧分数的倒数。
    static bool divide(const Fraction& left, const Fraction& right, Fraction& result) {
        if (right.numerator == 0) {
            return false;
        }
        Fraction reciprocal(right.denominator, right.numerator);
        return multiply(left, reciprocal, result);
    }

    static bool less(const Fraction& left, const Fraction& right) {
        return compare(left, right) < 0;
    }

    static bool equal(const Fraction& left, const Fraction& right) {
        return left.numerator == right.numerator && left.denominator == right.denominator;
    }

    // 按作业要求输出分数：整数、普通分数或带整数部分的混合分数。
    std::string toString() const {
        if (numerator == 0) {
            return "0";
        }

        const bool negative = numerator < 0;
        const std::uint64_t absoluteNumerator = absoluteValue(numerator);
        const std::uint64_t whole = absoluteNumerator / denominator;
        const std::uint64_t remainder = absoluteNumerator % denominator;

        std::string result;
        if (negative) {
            result += '-';
        }
        if (remainder == 0) {
            result += std::to_string(whole);
        } else if (whole == 0) {
            result += std::to_string(remainder);
            result += '/';
            result += std::to_string(denominator);
        } else {
            result += std::to_string(whole);
            result += u8"’";
            result += std::to_string(remainder);
            result += '/';
            result += std::to_string(denominator);
        }
        return result;
    }

private:
    // 比较两个分数时不直接交叉相乘，避免两个 int64_t 相乘溢出。
    // 这里使用连分数思想逐步比较整数部分和余数部分。
    static int compare(const Fraction& left, const Fraction& right) {
        if ((left.numerator < 0) != (right.numerator < 0)) {
            return left.numerator < 0 ? -1 : 1;
        }
        if (left.numerator == 0 && right.numerator == 0) {
            return 0;
        }

        const bool negative = left.numerator < 0;
        std::uint64_t leftNumerator = absoluteValue(left.numerator);
        std::uint64_t rightNumerator = absoluteValue(right.numerator);
        std::uint64_t leftDenominator = static_cast<std::uint64_t>(left.denominator);
        std::uint64_t rightDenominator = static_cast<std::uint64_t>(right.denominator);
        int direction = 1;
        while (true) {
            const std::uint64_t leftQuotient = leftNumerator / leftDenominator;
            const std::uint64_t rightQuotient = rightNumerator / rightDenominator;
            if (leftQuotient != rightQuotient) {
                const int answer = leftQuotient < rightQuotient ? -1 : 1;
                const int directedAnswer = direction * answer;
                return negative ? -directedAnswer : directedAnswer;
            }

            const std::uint64_t leftRemainder = leftNumerator % leftDenominator;
            const std::uint64_t rightRemainder = rightNumerator % rightDenominator;
            if (leftRemainder == 0 || rightRemainder == 0) {
                int answer = 0;
                if (leftRemainder == 0 && rightRemainder != 0) {
                    answer = -1;
                } else if (leftRemainder != 0 && rightRemainder == 0) {
                    answer = 1;
                }
                const int directedAnswer = direction * answer;
                return negative ? -directedAnswer : directedAnswer;
            }

            leftNumerator = leftDenominator;
            leftDenominator = leftRemainder;
            rightNumerator = rightDenominator;
            rightDenominator = rightRemainder;
            // 比较余数的倒数会反转大小关系，因此切换比较方向。
            direction = -direction;
        }
    }
};

// 表达式树中的四种二元运算。
enum class Operation { Add, Subtract, Multiply, Divide };

// 用于生成唯一性判断的内部运算符字符。
char operationChar(Operation operation) {
    switch (operation) {
    case Operation::Add:
        return '+';
    case Operation::Subtract:
        return '-';
    case Operation::Multiply:
        return '*';
    case Operation::Divide:
        return '/';
    }
    return '?';
}

// 用于写入 Exercises.txt 的显示符号。
std::string operationSymbol(Operation operation) {
    switch (operation) {
    case Operation::Add:
        return "+";
    case Operation::Subtract:
        return u8"−";
    case Operation::Multiply:
        return u8"×";
    case Operation::Divide:
        return u8"÷";
    }
    return "?";
}

// 只有加法和乘法允许交换左右子表达式而不改变题目含义。
bool isCommutative(Operation operation) {
    return operation == Operation::Add || operation == Operation::Multiply;
}

struct Expression {
    // Expression 是一棵表达式树：叶子节点保存数字，非叶子节点保存运算符和左右子树。
    bool isNumber = false;
    Fraction value;
    Operation operation = Operation::Add;
    std::unique_ptr<Expression> left;
    std::unique_ptr<Expression> right;
};

using ExpressionPtr = std::unique_ptr<Expression>;

ExpressionPtr makeNumber(const Fraction& value) {
    auto expression = std::make_unique<Expression>();
    expression->isNumber = true;
    expression->value = value;
    return expression;
}

// 计算一个二元节点，并同时检查题目的生成约束。
bool applyOperation(Operation operation,
                    const Fraction& left,
                    const Fraction& right,
                    Fraction& result) {
    switch (operation) {
    case Operation::Add:
        return Fraction::add(left, right, result);
    case Operation::Subtract:
        if (Fraction::less(left, right)) {
            return false;
        }
        return Fraction::subtract(left, right, result);
    case Operation::Multiply:
        return Fraction::multiply(left, right, result);
    case Operation::Divide:
        if (!Fraction::divide(left, right, result)) {
            return false;
        }
        // 每个除法子表达式的结果都必须是非零真分数。
        return result.numerator > 0 && result.numerator < result.denominator;
    }
    return false;
}

class ExpressionGenerator {
public:
    explicit ExpressionGenerator(std::uint64_t range)
        : range_(range), random_(std::random_device{}()) {}

    std::vector<ExpressionPtr> generate(std::size_t count) {
        std::vector<ExpressionPtr> expressions;
        expressions.reserve(count);
        std::unordered_set<std::string> uniqueKeys;
        uniqueKeys.reserve(count * 2 + 1);

        // 正常的 -r 范围可以很快生成大量不同题目；但在 -r 过小、组合数量不足时，
        // 随机重试可能永远无法达到目标数量。因此设置最大尝试次数，避免无限循环。
        const std::uint64_t maxAttempts =
            std::max<std::uint64_t>(200000, static_cast<std::uint64_t>(count) * 1000 + 100000);
        std::uint64_t attempts = 0;

        while (expressions.size() < count && attempts < maxAttempts) {
            ++attempts;
            const int operationCount = static_cast<int>(randomInt(1, 3));
            ExpressionPtr candidate = build(operationCount);
            if (!candidate) {
                continue;
            }

            const std::string key = canonical(candidate.get());
            if (uniqueKeys.insert(key).second) {
                expressions.push_back(std::move(candidate));
            }
        }

        if (expressions.size() != count) {
            throw std::runtime_error(
                "在给定的 -r 范围内无法生成足够多的不重复题目；请增大 -r 或减少 -n。\n"
                "程序已停止，避免在题目数量达到组合上限后无限重试。");
        }
        return expressions;
    }

private:
    std::uint64_t range_;
    std::mt19937_64 random_;

    std::uint64_t randomInt(std::uint64_t minimum, std::uint64_t maximum) {
        std::uniform_int_distribution<std::uint64_t> distribution(minimum, maximum);
        return distribution(random_);
    }

    // 按 -r 生成一个自然数、普通分数或混合分数。
    // 自然数和混合数的整数部分取 0 到 r-1，分母取 2 到 r-1。
    Fraction randomNumber() {
        const std::uint64_t maximumNatural = range_ - 1;
        const bool canMakeFraction = range_ >= 3;

        if (!canMakeFraction || randomInt(0, 99) < 60) {
            return Fraction(static_cast<std::int64_t>(randomInt(0, maximumNatural)), 1);
        }

        const std::uint64_t denominator = randomInt(2, range_ - 1);
        const std::uint64_t numerator = randomInt(1, denominator - 1);
        const std::uint64_t whole = randomInt(0, maximumNatural);
        std::int64_t mixedBase = 0;
        std::int64_t mixedNumerator = 0;
        Fraction result;
        if (!checkedMultiply(static_cast<std::int64_t>(whole),
                             static_cast<std::int64_t>(denominator),
                             mixedBase) ||
            !checkedAdd(mixedBase, static_cast<std::int64_t>(numerator), mixedNumerator) ||
            !Fraction::fromRaw(mixedNumerator,
                               static_cast<std::int64_t>(denominator),
                               result)) {
            // -r 已限制在 int64_t 范围内，这里只是对极端溢出情况做防御性处理。
            return Fraction(0, 1);
        }
        return result;
    }

    // 均匀随机选择一种运算符。
    Operation randomOperation() {
        switch (randomInt(0, 3)) {
        case 0:
            return Operation::Add;
        case 1:
            return Operation::Subtract;
        case 2:
            return Operation::Multiply;
        default:
            return Operation::Divide;
        }
    }

    // 递归生成恰好包含 operationCount 个运算符的表达式树。
    // 通过随机拆分左右子树的运算符数量，可以生成不同的括号结构。
    ExpressionPtr build(int operationCount) {
        if (operationCount == 0) {
            return makeNumber(randomNumber());
        }

        const int leftOperationCount = static_cast<int>(randomInt(0, operationCount - 1));
        const int rightOperationCount = operationCount - 1 - leftOperationCount;
        ExpressionPtr left = build(leftOperationCount);
        ExpressionPtr right = build(rightOperationCount);
        if (!left || !right) {
            return nullptr;
        }
        const Operation operation = randomOperation();

        Fraction result;
        if (!applyOperation(operation, left->value, right->value, result)) {
            // 当前候选违反了减法、除法或溢出约束，返回空指针让外层重新随机生成。
            return nullptr;
        }

        auto expression = std::make_unique<Expression>();
        expression->isNumber = false;
        expression->value = result;
        expression->operation = operation;
        expression->left = std::move(left);
        expression->right = std::move(right);
        return expression;
    }

    // 生成题目的规范键：加法和乘法节点的左右键排序，
    // 从而把可以通过交换左右子表达式得到的题目归为同一类。
    std::string canonical(const Expression* expression) const {
        if (expression->isNumber) {
            return "N" + std::to_string(expression->value.numerator) + "/" +
                   std::to_string(expression->value.denominator);
        }

        std::string left = canonical(expression->left.get());
        std::string right = canonical(expression->right.get());
        if (isCommutative(expression->operation) && right < left) {
            std::swap(left, right);
        }

        return "(" + std::string(1, operationChar(expression->operation)) + " " + left +
               " " + right + ")";
    }
};

int precedence(Operation operation) {
    return operation == Operation::Add || operation == Operation::Subtract ? 1 : 2;
}

// 按运算优先级输出表达式，并只保留会影响解析结果或表达式树结构的括号。
// parentPrecedence 表示父节点优先级，isRightChild 用于处理右侧同优先级子树。
std::string renderExpression(const Expression* expression,
                             int parentPrecedence = 0,
                             bool isRightChild = false) {
    if (expression->isNumber) {
        return expression->value.toString();
    }

    const int currentPrecedence = precedence(expression->operation);
    std::string result = renderExpression(expression->left.get(), currentPrecedence, false) +
                         " " + operationSymbol(expression->operation) + " " +
                         renderExpression(expression->right.get(), currentPrecedence, true);

    // 解析器采用左结合规则。右侧同优先级子树必须保留括号，
    // 例如 1 + (2 + 3) 不能输出成 1 + 2 + 3，否则表达式树会发生变化。
    const bool needsParentheses = currentPrecedence < parentPrecedence ||
                                  (currentPrecedence == parentPrecedence && isRightChild);
    if (needsParentheses) {
        result = "(" + result + ")";
    }
    return result;
}

std::string render(const Expression* expression) {
    return renderExpression(expression);
}

class Parser {
public:
    explicit Parser(std::string source) : source_(normalizeSymbols(std::move(source))) {}

    Fraction parse() {
        // 从最低优先级开始解析，最终要求整个字符串都已经被消费。
        Fraction result = parseAddSubtract();
        skipSpaces();
        if (position_ != source_.size()) {
            throw std::runtime_error("表达式中存在无法识别的内容：" + source_.substr(position_));
        }
        return result;
    }

private:
    std::string source_;
    std::size_t position_ = 0;

    void skipSpaces() {
        while (position_ < source_.size() &&
               std::isspace(static_cast<unsigned char>(source_[position_]))) {
            ++position_;
        }
    }

    // 如果当前位置是 expected，就消费该字符；允许运算符两侧存在空格。
    bool consume(char expected) {
        skipSpaces();
        if (position_ < source_.size() && source_[position_] == expected) {
            ++position_;
            return true;
        }
        return false;
    }

    // 读取一个非负整数，并检查是否超出 int64_t 可表示范围。
    std::int64_t readUnsignedInteger() {
        skipSpaces();
        const std::size_t start = position_;
        while (position_ < source_.size() &&
               std::isdigit(static_cast<unsigned char>(source_[position_]))) {
            ++position_;
        }
        if (start == position_) {
            throw std::runtime_error("需要一个自然数");
        }

        const std::string digits = source_.substr(start, position_ - start);
        try {
            const unsigned long long value = std::stoull(digits);
            if (value > static_cast<unsigned long long>(std::numeric_limits<std::int64_t>::max())) {
                throw std::runtime_error("数值超出程序支持的范围");
            }
            return static_cast<std::int64_t>(value);
        } catch (const std::invalid_argument&) {
            throw std::runtime_error("自然数格式错误：" + digits);
        } catch (const std::out_of_range&) {
            throw std::runtime_error("数值超出程序支持的范围");
        }
    }

    // 解析一个最基本的表达式：括号表达式、整数、普通分数或混合分数。
    Fraction parsePrimary() {
        skipSpaces();
        bool negative = false;
        if (position_ < source_.size() && source_[position_] == '-') {
            negative = true;
            ++position_;
        }
        if (consume('(')) {
            Fraction result = parseAddSubtract();
            if (!consume(')')) {
                throw std::runtime_error("缺少右括号");
            }
            return negative ? negate(result) : result;
        }

        const std::int64_t first = readUnsignedInteger();
        // 输出格式中的混合分数不在撇号两侧添加空格，例如 1’3/8。
        if (position_ < source_.size() && source_[position_] == '\'') {
            ++position_;
            const std::int64_t numerator = readUnsignedInteger();
            if (!consume('/')) {
                throw std::runtime_error("混合分数缺少分隔符 '/'");
            }
            const std::int64_t denominator = readUnsignedInteger();
            std::int64_t mixedBase = 0;
            std::int64_t mixedNumerator = 0;
            Fraction result;
            if (!checkedMultiply(first, denominator, mixedBase) ||
                !checkedAdd(mixedBase, numerator, mixedNumerator) ||
                !Fraction::fromRaw(mixedNumerator, denominator, result)) {
                throw std::runtime_error("混合分数超出程序支持的范围");
            }
            return negative ? negate(result) : result;
        }

        // 只有斜杠紧跟分子时才把 3/5 当作一个普通分数。
        // 生成的除法会写成 3 ÷ 5，转换后斜杠两侧有空格，因此仍会被识别为除法。
        if (position_ < source_.size() && source_[position_] == '/') {
            ++position_;
            const std::int64_t denominator = readUnsignedInteger();
            try {
                const Fraction result(first, denominator);
                return negative ? negate(result) : result;
            } catch (const std::exception&) {
                throw std::runtime_error("普通分数格式错误");
            }
        }
        const Fraction result(first, 1);
        return negative ? negate(result) : result;
    }

    Fraction negate(const Fraction& value) {
        std::int64_t numerator = 0;
        if (!checkedSubtract(0, value.numerator, numerator)) {
            throw std::runtime_error("负数答案超出程序支持的范围");
        }
        return Fraction(numerator, value.denominator);
    }

    // 解析乘法和除法，乘除具有高于加减的优先级。
    Fraction parseMultiplyDivide() {
        Fraction result = parsePrimary();
        while (true) {
            skipSpaces();
            if (position_ >= source_.size() ||
                (source_[position_] != '*' && source_[position_] != '/')) {
                break;
            }

            const char operation = source_[position_++];
            Fraction right = parsePrimary();
            Fraction next;
            const bool success = operation == '*'
                                     ? Fraction::multiply(result, right, next)
                                     : Fraction::divide(result, right, next);
            if (!success) {
                throw std::runtime_error(operation == '*' ? "乘法结果超出范围"
                                                          : "除数不能为 0");
            }
            result = next;
        }
        return result;
    }

    // 解析加法和减法；表达式按左结合规则计算，并拒绝产生负数的减法。
    Fraction parseAddSubtract() {
        Fraction result = parseMultiplyDivide();
        while (true) {
            skipSpaces();
            if (position_ >= source_.size() ||
                (source_[position_] != '+' && source_[position_] != '-')) {
                break;
            }

            const char operation = source_[position_++];
            Fraction right = parseMultiplyDivide();
            Fraction next;
            const bool success = operation == '+'
                                     ? Fraction::add(result, right, next)
                                     : Fraction::subtract(result, right, next);
            if (!success || (operation == '-' && Fraction::less(result, right))) {
                throw std::runtime_error(operation == '+' ? "加法结果超出范围" : "出现负数结果");
            }
            result = next;
        }
        return result;
    }
};

Fraction evaluate(const std::string& expression) {
    return Parser(expression).parse();
}

bool extractLineContent(const std::string& line, std::string& content) {
    const std::string cleaned = trim(line);
    if (cleaned.empty()) {
        return false;
    }

    // 兼容程序生成的“1. 表达式”、中文常用的“1、表达式”和全角句号格式。
    // 如果没有识别到编号前缀，就把整行当作不带编号的表达式或答案。
    std::size_t digitsEnd = 0;
    while (digitsEnd < cleaned.size() &&
           std::isdigit(static_cast<unsigned char>(cleaned[digitsEnd]))) {
        ++digitsEnd;
    }
    if (digitsEnd == 0) {
        content = cleaned;
        return true;
    }

    std::size_t separatorLength = 0;
    if (cleaned[digitsEnd] == '.') {
        separatorLength = 1;
    } else if (cleaned.compare(digitsEnd, std::string(u8"、").size(), u8"、") == 0) {
        separatorLength = std::string(u8"、").size();
    } else if (cleaned.compare(digitsEnd, std::string(u8"．").size(), u8"．") == 0) {
        separatorLength = std::string(u8"．").size();
    } else {
        content = cleaned;
        return true;
    }

    content = trim(cleaned.substr(digitsEnd + separatorLength));
    return !content.empty();
}

bool splitNumberedLine(const std::string& line, std::string& content) {
    // 保留这个包装函数，避免改变批改流程中已有的调用接口。
    if (!extractLineContent(line, content)) {
        return false;
    }
    return true;
}

std::vector<std::string> readLines(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::runtime_error("无法打开文件：" + path.string());
    }

    std::vector<std::string> lines;
    std::string line;
    while (std::getline(input, line)) {
        // 不跳过空行，以便把空白答案准确记为对应题目的错误答案，避免后续答案错位。
        lines.push_back(line);
    }
    return lines;
}

void writeExercises(const std::vector<ExpressionPtr>& expressions,
                    const std::filesystem::path& path) {
    std::ofstream output(path, std::ios::binary);
    if (!output) {
        throw std::runtime_error("无法写入文件：" + path.string());
    }

    // 每行写入题号、表达式和等号；答案单独写入 Answers.txt。
    for (std::size_t index = 0; index < expressions.size(); ++index) {
        output << index + 1 << ". " << render(expressions[index].get()) << " =\n";
    }
}

void writeAnswers(const std::vector<ExpressionPtr>& expressions,
                  const std::filesystem::path& path) {
    std::ofstream output(path, std::ios::binary);
    if (!output) {
        throw std::runtime_error("无法写入文件：" + path.string());
    }

    // 直接使用表达式树根节点中已经计算好的精确分数结果。
    for (std::size_t index = 0; index < expressions.size(); ++index) {
        output << index + 1 << ". " << expressions[index]->value.toString() << "\n";
    }
}

void writeGrade(const std::vector<std::size_t>& correct,
                const std::vector<std::size_t>& wrong,
                const std::filesystem::path& path) {
    std::ofstream output(path, std::ios::binary);
    if (!output) {
        throw std::runtime_error("无法写入文件：" + path.string());
    }

    // 统一输出 Correct 和 Wrong 两行，并列出对应的题号。
    auto writeResult = [&output](const char* label, const std::vector<std::size_t>& indexes) {
        output << label << ": " << indexes.size() << " (";
        for (std::size_t index = 0; index < indexes.size(); ++index) {
            if (index != 0) {
                output << ", ";
            }
            output << indexes[index];
        }
        output << ")\n";
    };

    writeResult("Correct", correct);
    output << "\n";
    writeResult("Wrong", wrong);
}

void grade(const std::filesystem::path& exercisePath,
           const std::filesystem::path& answerPath,
           const std::filesystem::path& gradePath) {
    // 题目文件和答案文件按行对应，先检查总行数是否一致。
    const std::vector<std::string> exerciseLines = readLines(exercisePath);
    const std::vector<std::string> answerLines = readLines(answerPath);
    if (exerciseLines.size() != answerLines.size()) {
        throw std::runtime_error("题目文件和答案文件的题目数量不一致");
    }

    std::vector<std::size_t> correct;
    std::vector<std::size_t> wrong;
    for (std::size_t index = 0; index < exerciseLines.size(); ++index) {
        std::string exercise;
        std::string answer;
        // 题目文件按规范应当合法，因此题目格式错误属于批改输入错误。
        if (!splitNumberedLine(exerciseLines[index], exercise)) {
            throw std::runtime_error("第 " + std::to_string(index + 1) + " 行题目为空或格式错误");
        }
        // 答案可能为空、带有非法字符或使用了错误表达式；这些情况都只算本题错误。
        if (!splitNumberedLine(answerLines[index], answer)) {
            wrong.push_back(index + 1);
            continue;
        }

        const std::size_t equals = exercise.rfind('=');
        if (equals == std::string::npos) {
            throw std::runtime_error("第 " + std::to_string(index + 1) + " 道题目缺少等号");
        }

        // 题目解析失败说明题目文件不符合规范，因此保留异常并停止批改。
        Fraction expected;
        try {
            expected = evaluate(trim(exercise.substr(0, equals)));
        } catch (const std::exception& error) {
            throw std::runtime_error("第 " + std::to_string(index + 1) + " 题目解析失败：" +
                                     error.what());
        }

        // 答案解析失败只影响当前题目，继续检查后面的答案。
        Fraction actual;
        try {
            actual = evaluate(answer);
        } catch (const std::exception&) {
            // 非法语法、除零等答案错误都归入 Wrong。
            wrong.push_back(index + 1);
            continue;
        }
        if (Fraction::equal(expected, actual)) {
            correct.push_back(index + 1);
        } else {
            wrong.push_back(index + 1);
        }
    }

    writeGrade(correct, wrong, gradePath);
}

void printHelp(const char* programName) {
    std::cout << "小学四则运算题目生成与批改程序\n\n"
              << "生成题目：\n"
              << "  " << programName << " -n 10 -r 10\n"
              << "  -n 题目数量，默认 10；-r 数值范围，必须给出且至少为 1。\n"
              << "  生成文件：当前目录下的 Exercises.txt 和 Answers.txt。\n\n"
              << "批改题目：\n"
              << "  " << programName << " -e Exercises.txt -a Answers.txt\n"
              << "  批改结果写入当前目录下的 Grade.txt。\n\n"
              << "说明：\n"
              << "  每题包含 1 到 3 个运算符；除法结果为非零真分数；\n"
              << "  生成范围中的自然数为 0 到 r-1，分母为 2 到 r-1。\n";
}

std::uint64_t parseUnsignedArgument(const std::string& value, const char* option) {
    if (value.empty() || value[0] == '-') {
        throw std::runtime_error(std::string(option) + " 必须是自然数");
    }
    try {
        std::size_t parsed = 0;
        const unsigned long long result = std::stoull(value, &parsed);
        if (parsed != value.size()) {
            throw std::runtime_error(std::string(option) + " 必须是自然数");
        }
        return result;
    } catch (const std::invalid_argument&) {
        throw std::runtime_error(std::string(option) + " 必须是自然数");
    } catch (const std::out_of_range&) {
        throw std::runtime_error(std::string(option) + " 数值过大");
    }
}

struct Arguments {
    // 保存命令行解析结果；程序有“生成题目”和“批改答案”两种互斥模式。
    bool help = false;
    bool grading = false;
    bool rangeGiven = false;
    std::size_t count = 10;
    std::uint64_t range = 0;
    std::filesystem::path exerciseFile;
    std::filesystem::path answerFile;
};

Arguments parseArguments(int argc, char* argv[]) {
    Arguments arguments;
    bool exerciseGiven = false;
    bool answerGiven = false;

    // 逐个读取选项，并在发现缺少参数或未知参数时立即提示用户。
    for (int index = 1; index < argc; ++index) {
        const std::string option = argv[index];
        if (option == "-h" || option == "--help") {
            arguments.help = true;
        } else if (option == "-n") {
            if (++index >= argc) {
                throw std::runtime_error("-n 缺少参数");
            }
            const std::uint64_t count = parseUnsignedArgument(argv[index], "-n");
            if (count > std::numeric_limits<std::size_t>::max()) {
                throw std::runtime_error("-n 数值过大");
            }
            arguments.count = static_cast<std::size_t>(count);
        } else if (option == "-r") {
            if (++index >= argc) {
                throw std::runtime_error("-r 缺少参数");
            }
            arguments.range = parseUnsignedArgument(argv[index], "-r");
            arguments.rangeGiven = true;
        } else if (option == "-e") {
            if (++index >= argc) {
                throw std::runtime_error("-e 缺少题目文件路径");
            }
            arguments.exerciseFile = argv[index];
            exerciseGiven = true;
        } else if (option == "-a") {
            if (++index >= argc) {
                throw std::runtime_error("-a 缺少答案文件路径");
            }
            arguments.answerFile = argv[index];
            answerGiven = true;
        } else {
            throw std::runtime_error("未知参数：" + option);
        }
    }

    if (arguments.help) {
        return arguments;
    }

    if (exerciseGiven || answerGiven) {
        if (!exerciseGiven || !answerGiven) {
            throw std::runtime_error("批改模式必须同时给出 -e 和 -a");
        }
        arguments.grading = true;
        if (arguments.rangeGiven) {
            throw std::runtime_error("-e/-a 批改模式不能同时使用 -r");
        }
        return arguments;
    }

    if (!arguments.rangeGiven) {
        throw std::runtime_error("生成题目时必须给出 -r 参数");
    }
    if (arguments.range == 0) {
        throw std::runtime_error("-r 必须是至少为 1 的自然数");
    }
    if (arguments.range > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
        throw std::runtime_error("-r 数值过大，必须不超过 int64_t 的最大值");
    }
    return arguments;
}

} // 匿名命名空间

int main(int argc, char* argv[]) {
    try {
        const Arguments arguments = parseArguments(argc, argv);
        if (arguments.help) {
            printHelp(argc > 0 ? argv[0] : "Myapp");
            return 0;
        }

        const std::filesystem::path currentDirectory = std::filesystem::current_path();
        if (arguments.grading) {
            grade(arguments.exerciseFile, arguments.answerFile, currentDirectory / "Grade.txt");
            std::cout << "批改完成，结果已写入：" << (currentDirectory / "Grade.txt") << "\n";
            return 0;
        }

        ExpressionGenerator generator(arguments.range);
        const std::vector<ExpressionPtr> expressions = generator.generate(arguments.count);
        writeExercises(expressions, currentDirectory / "Exercises.txt");
        writeAnswers(expressions, currentDirectory / "Answers.txt");
        std::cout << "已生成 " << expressions.size() << " 道题目。\n"
                  << "题目文件：" << (currentDirectory / "Exercises.txt") << "\n"
                  << "答案文件：" << (currentDirectory / "Answers.txt") << "\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "错误：" << error.what() << "\n\n";
        printHelp(argc > 0 ? argv[0] : "Myapp");
        return 1;
    }
}
