#include "pch_engine.h"
#include "MathExpression.h"

namespace ShaderLab::Rendering
{
	namespace
	{
		std::atomic<MathExpressionEvaluator> sEvaluator{ nullptr };
	}

	void SetMathExpressionEvaluator(MathExpressionEvaluator evaluator)
	{
		sEvaluator.store(evaluator, std::memory_order_release);
	}

	bool EvaluateMathExpression(
		const std::wstring& expression,
		const wchar_t* const* varNames,
		const float* varValues,
		size_t varCount,
		float& outResult,
		std::wstring* outError)
	{
		if (const auto evaluator = sEvaluator.load(std::memory_order_acquire))
			return evaluator(expression, varNames, varValues, varCount, outResult, outError);
		outResult = 0.0f;
		if (outError) *outError = L"No expression evaluator: Numeric Expression nodes need ShaderLab's";
		return false;
	}
}
