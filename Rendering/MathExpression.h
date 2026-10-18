#pragma once
#include <string>

namespace ShaderLab::Rendering
{
	// Evaluates a Numeric Expression node's expression. The expression may
	// reference scalar variables A,B,C,D,E via the parallel arrays. Returns
	// true on success and writes the result; on parse/eval failure returns
	// false with an optional error message. The engine carries no expression
	// language: a host registers one with SetMathExpressionEvaluator
	// (ShaderLab registers ExprTk), and without one every expression fails
	// with a message saying so.
	bool EvaluateMathExpression(
		const std::wstring& expression,
		const wchar_t* const* varNames,
		const float* varValues,
		size_t varCount,
		float& outResult,
		std::wstring* outError = nullptr);

	using MathExpressionEvaluator = bool (*)(
		const std::wstring& expression,
		const wchar_t* const* varNames,
		const float* varValues,
		size_t varCount,
		float& outResult,
		std::wstring* outError);

	// Process-wide; set once at startup, before any graph evaluates.
	void SetMathExpressionEvaluator(MathExpressionEvaluator evaluator);
}
