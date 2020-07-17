#include <SHADERed/Objects/Debug/ExpressionCompiler.h>
#include <common/BinaryFileReader.h>
#include <common/BinaryVectorWriter.h>
#include <spvgentwo/GLSL450Instruction.h>

namespace ed {
	template <typename U32Vector>
	class BinaryVectorReader : public spvgentwo::IReader {
	public:
		BinaryVectorReader(U32Vector& spv) : m_vec(spv), m_index(0) { }
		~BinaryVectorReader() { }

		bool get(unsigned int& _word) final
		{
			if (m_index >= m_vec.size())
				return false;
			_word = m_vec[m_index++];
			return true;
		}

	private:
		U32Vector& m_vec;
		int m_index;
	};

	ExpressionCompiler::ExpressionCompiler()
	{
		m_error = false;
		m_module = nullptr;
	}
	ExpressionCompiler::~ExpressionCompiler()
	{
		// crashes: [TODO: memory leak here]
		// delete m_module;
	}
	int ExpressionCompiler::Compile(const std::string& expr, std::vector<unsigned int>& spv)
	{
		if (expr.empty())
			return -1;

		spvgentwo::HeapAllocator alloc;

		BinaryVectorReader reader(spv);
		spvgentwo::Grammar m_grammar(&alloc);

		if (m_module) delete m_module;

		m_module = new spvgentwo::Module(&alloc, spvgentwo::spv::Version);

		m_module->read(&reader, m_grammar);
		m_module->resolveIDs();
		m_module->reconstructTypeAndConstantInfo();
		m_module->reconstructNames();

		m_func = &m_module->addFunction<void>("$$_shadered_immediate", spvgentwo::spv::FunctionControlMask::Const);

		// parse expression
		expr::Parser parser(expr.c_str(), expr.size());
		expr::Node* root = parser.Parse();
		if (parser.Error())
			return -1;

		// get variables
		m_vars.clear();
		for (expr::Node* n : parser.GetList())
			if (n->GetNodeType() == expr::NodeType::Identifier)
				m_vars[((expr::IdentifierNode*)n)->Name] = nullptr;
		

		// I know this is super hacky but meh, it works
		m_module->iterateInstructions([&](spvgentwo::Instruction& inst) {
			const char* iName = m_module->getName(&inst, 0);
			if (m_vars.count(iName))
				m_vars[iName] = &inst;
		});

		// check if a non existing variable is being used
		for (const auto& var : m_vars)
			if (var.second == nullptr)
				return -1;

		spvgentwo::Instruction* inst = m_visit(root);

		if (m_error || inst == nullptr)
			return -1;

		// add op return
		spvgentwo::BasicBlock& bb = *(*m_func);
		bb->opReturn();

		// assign IDs and clear memory
		m_module->assignIDs();
		parser.Clear();

		// output spirv
		spv.clear();
		spvgentwo::BinaryVectorWriter writer(spv);
		m_module->write(&writer);

		// return result's ID
		return (int)inst->getResultId();
	}
	std::vector<std::string> ExpressionCompiler::GetVariableList()
	{
		std::vector<std::string> ret;
		for (const auto& var : m_vars)
			ret.push_back(var.first);
		return ret;
	}

	spvgentwo::Instruction* ExpressionCompiler::m_visit(expr::Node* node)
	{
		if (m_error)
			return nullptr;
		if (node == nullptr) {
			m_error = true;
			return nullptr;
		}

		spvgentwo::BasicBlock& bb = *(*m_func);

		switch (node->GetNodeType()) {
		case expr::NodeType::BinaryExpression: {
			expr::BinaryExpressionNode* bexpr = ((expr::BinaryExpressionNode*)node);

			spvgentwo::Instruction* leftInstr = m_visit(bexpr->Left);
			spvgentwo::Instruction* rightInstr = m_visit(bexpr->Right);

			if (leftInstr == nullptr || rightInstr == nullptr) {
				m_error = true;
				return nullptr;
			}

			m_convert(bexpr->Operator, leftInstr, rightInstr, leftInstr, rightInstr);

			if (leftInstr->getType()->isVector() && rightInstr->getType()->isVector())
				if (leftInstr->getType()->getVectorComponentCount() != rightInstr->getType()->getVectorComponentCount()) {
					m_error = true;
					return nullptr;
				}

			if (leftInstr->getType()->getBaseType().isFloat()) {
				if (bexpr->Operator == '+')
					return bb->opFAdd(leftInstr, rightInstr);
				else if (bexpr->Operator == '*') {
					if (leftInstr->getType()->isVector() && rightInstr->getType()->isScalar())
						return bb->opVectorTimesScalar(leftInstr, rightInstr);
					else if (rightInstr->getType()->isVector() && leftInstr->getType()->isScalar())
						return bb->opVectorTimesScalar(rightInstr, leftInstr);
					else if (leftInstr->getType()->isVector() && rightInstr->getType()->isMatrix())
						return bb->opVectorTimesMatrix(leftInstr, rightInstr);
					else if (rightInstr->getType()->isVector() && leftInstr->getType()->isMatrix())
						return bb->opMatrixTimesVector(rightInstr, leftInstr);
					else if (rightInstr->getType()->isMatrix() && leftInstr->getType()->isMatrix())
						return bb->opMatrixTimesMatrix(rightInstr, leftInstr);
					return bb->opFMul(leftInstr, rightInstr);
				} else if (bexpr->Operator == '-')
					return bb->opFSub(leftInstr, rightInstr);
				else if (bexpr->Operator == '/')
					return bb->opFDiv(leftInstr, rightInstr);
				else if (bexpr->Operator == '%')
					return bb->opFMod(leftInstr, rightInstr);
				else if (bexpr->Operator == '<')
					return bb->opFOrdLessThan(leftInstr, rightInstr);
				else if (bexpr->Operator == '>')
					return bb->opFOrdGreaterThan(leftInstr, rightInstr);
				else if (bexpr->Operator == expr::TokenType_Equal)
					return bb->opFOrdEqual(leftInstr, rightInstr);
				else if (bexpr->Operator == expr::TokenType_NotEqual)
					return bb->opFOrdNotEqual(leftInstr, rightInstr);
				else if (bexpr->Operator == expr::TokenType_LessThanEqual)
					return bb->opFOrdLessThanEqual(leftInstr, rightInstr);
				else if (bexpr->Operator == expr::TokenType_GreaterThanEqual)
					return bb->opFOrdGreaterThanEqual(leftInstr, rightInstr);
			} else {
				if (bexpr->Operator == '+')
					return bb->opIAdd(leftInstr, rightInstr);
				else if (bexpr->Operator == '*')
					return bb->opIMul(leftInstr, rightInstr);
				else if (bexpr->Operator == '-')
					return bb->opISub(leftInstr, rightInstr);
				else if (bexpr->Operator == '/')
					return bb->opSDiv(leftInstr, rightInstr);
				else if (bexpr->Operator == '%')
					return bb->opSMod(leftInstr, rightInstr);
				else if (bexpr->Operator == '<')
					return bb->opSLessThan(leftInstr, rightInstr);
				else if (bexpr->Operator == '>')
					return bb->opSGreaterThan(leftInstr, rightInstr);
				else if (bexpr->Operator == '&')
					return bb->opBitwiseAnd(leftInstr, rightInstr);
				else if (bexpr->Operator == '|')
					return bb->opBitwiseOr(leftInstr, rightInstr);
				else if (bexpr->Operator == '^')
					return bb->opBitwiseXor(leftInstr, rightInstr);
				else if (bexpr->Operator == expr::TokenType_BitshiftLeft) {
					return bb->opShiftLeftLogical(leftInstr, rightInstr);
				} else if (bexpr->Operator == expr::TokenType_BitshiftRight) {
					if (leftInstr->getType()->isSigned())
						return bb->opShiftRightArithmetic(leftInstr, rightInstr);
					else
						return bb->opShiftRightLogical(leftInstr, rightInstr);
				} else if (bexpr->Operator == expr::TokenType_LogicOr)
					return bb->opLogicalOr(leftInstr, rightInstr);
				else if (bexpr->Operator == expr::TokenType_LogicAnd)
					return bb->opLogicalAnd(leftInstr, rightInstr);
				else if (bexpr->Operator == expr::TokenType_Equal)
					return bb->opIEqual(leftInstr, rightInstr);
				else if (bexpr->Operator == expr::TokenType_NotEqual)
					return bb->opINotEqual(leftInstr, rightInstr);
				else if (bexpr->Operator == expr::TokenType_LessThanEqual)
					return bb->opSLessThanEqual(leftInstr, rightInstr);
				else if (bexpr->Operator == expr::TokenType_GreaterThanEqual)
					return bb->opSGreaterThanEqual(leftInstr, rightInstr);
			}
		} break;
		case expr::NodeType::TernaryExpression: {
			expr::TernaryExpressionNode* texpr = ((expr::TernaryExpressionNode*)node);

			spvgentwo::Instruction* conditionInstr = m_visit(texpr->Condition);
			spvgentwo::Instruction* onTrueInstr = m_visit(texpr->OnTrue);
			spvgentwo::Instruction* onFalseInstr = m_visit(texpr->OnFalse);

			if (conditionInstr == nullptr || onTrueInstr == nullptr || onFalseInstr == nullptr) {
				m_error = true;
				return nullptr;
			}

			return bb->opSelect(conditionInstr, onTrueInstr, onFalseInstr);
		} break;
		case expr::NodeType::IntegerLiteral:
			return m_module->constant(((expr::IntegerLiteralNode*)node)->Value);
			break;
		case expr::NodeType::FloatLiteral:
			return m_module->constant(((expr::FloatLiteralNode*)node)->Value);
			break;
		case expr::NodeType::BooleanLiteral:
			return m_module->constant(((expr::BooleanLiteralNode*)node)->Value);
			break;
		case expr::NodeType::Identifier: {
			const char* name = ((expr::IdentifierNode*)node)->Name;
			if (m_opLoads.count(name) == 0)
				m_opLoads[name] = bb->opLoad(m_vars[name]);

			return m_opLoads[name];
		} break;
		case expr::NodeType::UnaryExpression: {
			expr::UnaryExpressionNode* uexpr = ((expr::UnaryExpressionNode*)node);
			spvgentwo::Instruction* childInstr = m_visit(uexpr->Child);

			if (childInstr == nullptr) {
				m_error = true;
				return nullptr;
			}

			if (childInstr->getType()->getBaseType().isFloat()) {
				if (uexpr->Operator == '-')
					return bb->opFNegate(childInstr);
				else if (uexpr->Operator == expr::TokenType_Increment) {
					spvgentwo::Instruction* ret = bb->opFAdd(childInstr, m_module->constant<float>(1.0f));
					if (uexpr->IsPost)
						return childInstr;
					else
						return ret;
				} else if (uexpr->Operator == expr::TokenType_Decrement) {
					spvgentwo::Instruction* ret = bb->opFSub(childInstr, m_module->constant<float>(1.0f));
					if (uexpr->IsPost)
						return childInstr;
					else
						return ret;
				}
			} else {
				if (uexpr->Operator == '-')
					return bb->opSNegate(childInstr);
				else if (uexpr->Operator == '!')
					return bb->opLogicalNot(childInstr);
				else if (uexpr->Operator == '~')
					return bb->opNot(childInstr);
				else if (uexpr->Operator == expr::TokenType_Increment) {
					spvgentwo::Instruction* ret = bb->opIAdd(childInstr, m_module->constant<int>(1));
					if (uexpr->IsPost)
						return childInstr;
					else
						return ret;
				} else if (uexpr->Operator == expr::TokenType_Decrement) {
					spvgentwo::Instruction* ret = bb->opISub(childInstr, m_module->constant<int>(1));
					if (uexpr->IsPost)
						return childInstr;
					else
						return ret;
				}
			}
		} break;
		case expr::NodeType::Cast: {
			expr::CastNode* cast = (expr::CastNode*)node;
			spvgentwo::Instruction* childInstr = m_visit(cast->Object);

			if (childInstr == nullptr) {
				m_error = true;
				return nullptr;
			}

			switch (cast->Type) {
			case expr::TokenType_Int:
			case expr::TokenType_Int2:
			case expr::TokenType_Int3:
			case expr::TokenType_Int4: {
				if (childInstr->getType()->getBaseType().isFloat())
					return bb->opConvertFToS(childInstr);
			} break;
			case expr::TokenType_Uint:
			case expr::TokenType_Uint2:
			case expr::TokenType_Uint3:
			case expr::TokenType_Uint4: {
				if (childInstr->getType()->getBaseType().isFloat())
					return bb->opConvertFToU(childInstr);
			} break;
			case expr::TokenType_Float:
			case expr::TokenType_Float2:
			case expr::TokenType_Float3:
			case expr::TokenType_Float4: {
				if (childInstr->getType()->getBaseType().isSInt())
					return bb->opConvertSToF(childInstr);
				else if (childInstr->getType()->getBaseType().isUInt())
					return bb->opConvertUToF(childInstr);
			} break;
			}

			return childInstr; // skip unnecessary
		} break;
		case expr::NodeType::FunctionCall: {
			expr::FunctionCallNode* fcall = (expr::FunctionCallNode*)node;
			int tok = fcall->TokenType;
			std::string fname(fcall->Name);
			std::vector<spvgentwo::Instruction*> args(fcall->Arguments.size(), nullptr);

			for (int i = 0; i < args.size(); i++) {
				args[i] = m_visit(fcall->Arguments[i]);
				if (args[i] == nullptr) {
					m_error = true;
					return nullptr;
				}
			}

			for (auto& func : m_module->getFunctions()) {
				std::string userFName(m_module->getName(func.getFunction(), 0));
				userFName = userFName.substr(0, userFName.find_first_of('('));

				if (userFName == fname && args.size() > 0)
					return bb->call(&func, args[0]); // TODO
			}

			if (args.size() == 0) {
				m_error = true;
				return nullptr;
			}

			if (tok == expr::TokenType_Int) {
				if (args[0]->getType()->getBaseType().isFloat())
					return bb->opConvertFToS(args[0]);
				return args[0];
			} else if (tok == expr::TokenType_Uint) {
				if (args[0]->getType()->getBaseType().isFloat())
					return bb->opConvertFToU(args[0]);
				return args[0];
			} else if (tok == expr::TokenType_Float) {
				if (args[0]->getType()->getBaseType().isSInt())
					return bb->opConvertSToF(args[0]);
				else if (args[0]->getType()->getBaseType().isUInt())
					return bb->opConvertUToF(args[0]);
				return args[0];
			} else if (m_isVector(tok))
				return m_constructVector(m_getBaseType(tok), m_getCompCount(tok), args);
			else if (fname == "round")
				return bb.ext<spvgentwo::ext::GLSL>()->opRound(m_simpleConvert(expr::TokenType_Float, args[0]));
			else if (fname == "roundEven")
				return bb.ext<spvgentwo::ext::GLSL>()->opRoundEven(m_simpleConvert(expr::TokenType_Float, args[0]));
			else if (fname == "trunc")
				return bb.ext<spvgentwo::ext::GLSL>()->opTrunc(m_simpleConvert(expr::TokenType_Float, args[0]));
			else if (fname == "abs") {
				if (args[0]->getType()->getBaseType().isFloat())
					return bb.ext<spvgentwo::ext::GLSL>()->opFAbs(args[0]);
				else
					return bb.ext<spvgentwo::ext::GLSL>()->opSAbs(args[0]);
			} else if (fname == "sign") {
				if (args[0]->getType()->getBaseType().isFloat())
					return bb.ext<spvgentwo::ext::GLSL>()->opFSign(args[0]);
				else
					return bb.ext<spvgentwo::ext::GLSL>()->opSSign(args[0]);
			} else if (fname == "floor")
				return bb.ext<spvgentwo::ext::GLSL>()->opFloor(m_simpleConvert(expr::TokenType_Float, args[0]));
			else if (fname == "ceil")
				return bb.ext<spvgentwo::ext::GLSL>()->opCeil(m_simpleConvert(expr::TokenType_Float, args[0]));
			else if (fname == "fract" || fname == "frac") // TODO: HLSL's frac =/= GLSL's fract AFAIK
				return bb.ext<spvgentwo::ext::GLSL>()->opFract(m_simpleConvert(expr::TokenType_Float, args[0]));
			else if (fname == "radians")
				return bb.ext<spvgentwo::ext::GLSL>()->opRadians(m_simpleConvert(expr::TokenType_Float, args[0]));
			else if (fname == "degrees")
				return bb.ext<spvgentwo::ext::GLSL>()->opDegrees(m_simpleConvert(expr::TokenType_Float, args[0]));
			else if (fname == "sin")
				return bb.ext<spvgentwo::ext::GLSL>()->opSin(m_simpleConvert(expr::TokenType_Float, args[0]));
			else if (fname == "cos")
				return bb.ext<spvgentwo::ext::GLSL>()->opCos(m_simpleConvert(expr::TokenType_Float, args[0]));
			else if (fname == "tan")
				return bb.ext<spvgentwo::ext::GLSL>()->opTan(m_simpleConvert(expr::TokenType_Float, args[0]));
			else if (fname == "asin")
				return bb.ext<spvgentwo::ext::GLSL>()->opASin(m_simpleConvert(expr::TokenType_Float, args[0]));
			else if (fname == "acos")
				return bb.ext<spvgentwo::ext::GLSL>()->opACos(m_simpleConvert(expr::TokenType_Float, args[0]));
			else if (fname == "atan" || fname == "atan2") {
				if (args.size() == 1)
					return bb.ext<spvgentwo::ext::GLSL>()->opATan(m_simpleConvert(expr::TokenType_Float, args[0]));
				else
					return bb.ext<spvgentwo::ext::GLSL>()->opAtan2(m_simpleConvert(expr::TokenType_Float, args[0]), m_simpleConvert(expr::TokenType_Float, args[1]));
			} else if (fname == "sinh")
				return bb.ext<spvgentwo::ext::GLSL>()->opSinh(m_simpleConvert(expr::TokenType_Float, args[0]));
			else if (fname == "cosh")
				return bb.ext<spvgentwo::ext::GLSL>()->opCosh(m_simpleConvert(expr::TokenType_Float, args[0]));
			else if (fname == "tanh")
				return bb.ext<spvgentwo::ext::GLSL>()->opTanh(m_simpleConvert(expr::TokenType_Float, args[0]));
			else if (fname == "asinh")
				return bb.ext<spvgentwo::ext::GLSL>()->opAsinh(m_simpleConvert(expr::TokenType_Float, args[0]));
			else if (fname == "acosh")
				return bb.ext<spvgentwo::ext::GLSL>()->opAcosh(m_simpleConvert(expr::TokenType_Float, args[0]));
			else if (fname == "atanh")
				return bb.ext<spvgentwo::ext::GLSL>()->opAtanh(m_simpleConvert(expr::TokenType_Float, args[0]));
			else if (fname == "pow" && args.size() == 2)
				return bb.ext<spvgentwo::ext::GLSL>()->opPow(m_simpleConvert(expr::TokenType_Float, args[0]), m_simpleConvert(expr::TokenType_Float, args[1]));
			else if (fname == "exp")
				return bb.ext<spvgentwo::ext::GLSL>()->opExp(m_simpleConvert(expr::TokenType_Float, args[0]));
			else if (fname == "exp2")
				return bb.ext<spvgentwo::ext::GLSL>()->opExp2(m_simpleConvert(expr::TokenType_Float, args[0]));
			else if (fname == "log")
				return bb.ext<spvgentwo::ext::GLSL>()->opLog(m_simpleConvert(expr::TokenType_Float, args[0]));
			else if (fname == "log2")
				return bb.ext<spvgentwo::ext::GLSL>()->opLog2(m_simpleConvert(expr::TokenType_Float, args[0]));
			else if (fname == "sqrt")
				return bb.ext<spvgentwo::ext::GLSL>()->opSqrt(m_simpleConvert(expr::TokenType_Float, args[0]));
			else if (fname == "rsqrt" || fname == "inversesqrt")
				return bb.ext<spvgentwo::ext::GLSL>()->opInverseSqrt(m_simpleConvert(expr::TokenType_Float, args[0]));
			else if (fname == "determinant")
				return bb.ext<spvgentwo::ext::GLSL>()->opDeterminant(args[0]);
			else if (fname == "inverse")
				return bb.ext<spvgentwo::ext::GLSL>()->opMatrixInverse(args[0]);
			else if (fname == "min" && args.size() == 2) {
				if (args[0]->getType()->getBaseType().isFloat() || args[1]->getType()->getBaseType().isFloat())
					return bb.ext<spvgentwo::ext::GLSL>()->opFMin(m_simpleConvert(expr::TokenType_Float, args[0]), m_simpleConvert(expr::TokenType_Float, args[1]));
				else if (args[0]->getType()->getBaseType().isSInt())
					return bb.ext<spvgentwo::ext::GLSL>()->opSMin(m_simpleConvert(expr::TokenType_Int, args[0]), m_simpleConvert(expr::TokenType_Int, args[1]));
				else if (args[0]->getType()->getBaseType().isUInt())
					return bb.ext<spvgentwo::ext::GLSL>()->opUMin(m_simpleConvert(expr::TokenType_Uint, args[0]), m_simpleConvert(expr::TokenType_Uint, args[1]));
			} else if (fname == "max" && args.size() == 2) {
				if (args[0]->getType()->getBaseType().isFloat() || args[1]->getType()->getBaseType().isFloat())
					return bb.ext<spvgentwo::ext::GLSL>()->opFMax(m_simpleConvert(expr::TokenType_Float, args[0]), m_simpleConvert(expr::TokenType_Float, args[1]));
				else if (args[0]->getType()->getBaseType().isSInt())
					return bb.ext<spvgentwo::ext::GLSL>()->opSMax(m_simpleConvert(expr::TokenType_Int, args[0]), m_simpleConvert(expr::TokenType_Int, args[1]));
				else if (args[0]->getType()->getBaseType().isUInt())
					return bb.ext<spvgentwo::ext::GLSL>()->opUMax(m_simpleConvert(expr::TokenType_Uint, args[0]), m_simpleConvert(expr::TokenType_Uint, args[1]));
			} else if (fname == "clamp" && args.size() == 3) {
				if (args[0]->getType()->getBaseType().isFloat() || args[1]->getType()->getBaseType().isFloat() || args[2]->getType()->getBaseType().isFloat())
					return bb.ext<spvgentwo::ext::GLSL>()->opFClamp(m_simpleConvert(expr::TokenType_Float, args[0]), m_simpleConvert(expr::TokenType_Float, args[1]), m_simpleConvert(expr::TokenType_Float, args[2]));
				else if (args[0]->getType()->getBaseType().isSInt())
					return bb.ext<spvgentwo::ext::GLSL>()->opSClamp(m_simpleConvert(expr::TokenType_Int, args[0]), m_simpleConvert(expr::TokenType_Int, args[1]), m_simpleConvert(expr::TokenType_Int, args[2]));
				else if (args[0]->getType()->getBaseType().isUInt())
					return bb.ext<spvgentwo::ext::GLSL>()->opUClamp(m_simpleConvert(expr::TokenType_Uint, args[0]), m_simpleConvert(expr::TokenType_Uint, args[1]), m_simpleConvert(expr::TokenType_Uint, args[2]));
			} else if (fname == "mix" && args.size() == 3) {
				if (args[0]->getType()->getBaseType().isFloat() || args[1]->getType()->getBaseType().isFloat() || args[2]->getType()->getBaseType().isFloat())
					return bb.ext<spvgentwo::ext::GLSL>()->opFMix(m_simpleConvert(expr::TokenType_Float, args[0]), m_simpleConvert(expr::TokenType_Float, args[1]), m_simpleConvert(expr::TokenType_Float, args[2]));
				else {
					m_error = true;
					return nullptr;
				}
			} else if (fname == "step" && args.size() == 2)
				return bb.ext<spvgentwo::ext::GLSL>()->opStep(m_simpleConvert(expr::TokenType_Float, args[0]), m_simpleConvert(expr::TokenType_Float, args[1]));
			else if (fname == "smoothstep" && args.size() == 3)
				return bb.ext<spvgentwo::ext::GLSL>()->opSmoothStep(m_simpleConvert(expr::TokenType_Float, args[0]), m_simpleConvert(expr::TokenType_Float, args[1]), m_simpleConvert(expr::TokenType_Float, args[2]));
			else if (fname == "fma" && args.size() == 3)
				return bb.ext<spvgentwo::ext::GLSL>()->opFma(m_simpleConvert(expr::TokenType_Float, args[0]), m_simpleConvert(expr::TokenType_Float, args[1]), m_simpleConvert(expr::TokenType_Float, args[2]));
			else if (fname == "cross" && args.size() == 2)
				return bb.ext<spvgentwo::ext::GLSL>()->opCross(m_simpleConvert(expr::TokenType_Float, args[0]), m_simpleConvert(expr::TokenType_Float, args[1]));
			else if (fname == "dot" && args.size() == 2)
				return bb->opDot(m_simpleConvert(expr::TokenType_Float, args[0]), m_simpleConvert(expr::TokenType_Float, args[1]));
			else if (fname == "any")
				return bb->opAny(args[0]);
			else if (fname == "all")
				return bb->opAll(args[0]);
			else if (fname == "ddx" || fname == "dFdx")
				return bb->opDPdx(m_simpleConvert(expr::TokenType_Float, args[0]));
			else if (fname == "ddy" || fname == "dFdy")
				return bb->opDPdy(m_simpleConvert(expr::TokenType_Float, args[0]));
			else if (fname == "fwidth")
				return bb->opFwidth(m_simpleConvert(expr::TokenType_Float, args[0]));
			else if (fname == "ddx_fine" || fname == "dFdxFine")
				return bb->opDPdxFine(m_simpleConvert(expr::TokenType_Float, args[0]));
			else if (fname == "ddy_fine" || fname == "dFdyFine")
				return bb->opDPdyFine(m_simpleConvert(expr::TokenType_Float, args[0]));
			else if (fname == "fwidthFine")
				return bb->opFwidthFine(m_simpleConvert(expr::TokenType_Float, args[0]));
			else if (fname == "ddx_coarse" || fname == "dFdxCoarse")
				return bb->opDPdxCoarse(m_simpleConvert(expr::TokenType_Float, args[0]));
			else if (fname == "ddy_coarse" || fname == "dFdyCoarse")
				return bb->opDPdyCoarse(m_simpleConvert(expr::TokenType_Float, args[0]));
			else if (fname == "fwidthCoarse")
				return bb->opFwidthCoarse(m_simpleConvert(expr::TokenType_Float, args[0]));
			else if (fname == "texture")
				return bb->opImageSampleImplictLod(args[0], m_simpleConvert(expr::TokenType_Float, args[1]));
			else {
				m_error = true;
				return nullptr;
			}
		} break;
		case expr::NodeType::MemberAccess: {
			expr::MemberAccessNode* maccess = (expr::MemberAccessNode*)node;
			spvgentwo::Instruction* obj = m_visit(maccess->Object);
			if (obj == nullptr) {
				m_error = true;
				return nullptr;
			}
			if (obj->getType()->isVector())
				return m_swizzle(obj, maccess->Field);
		} break;
		case expr::NodeType::MethodCall: {
			expr::MethodCallNode* mcall = (expr::MethodCallNode*)node;
			std::string fname(mcall->Name);

			spvgentwo::Instruction* obj = m_visit(mcall->Object);
			std::vector<spvgentwo::Instruction*> args(mcall->Arguments.size(), nullptr);

			for (int i = 0; i < args.size(); i++) {
				args[i] = m_visit(mcall->Arguments[i]);
				if (args[i] == nullptr) {
					m_error = true;
					return nullptr;
				}
			}

			// HLSL stuff:
			if (obj->getType()->isSampledImage()) {
				if (fname == "Sample" && args.size() > 1)
					return bb->opImageSampleImplictLod(obj, m_simpleConvert(expr::TokenType_Float, args[1]));
			}
		} break;
		}

		return nullptr;
	}
	void ExpressionCompiler::m_convert(int op, spvgentwo::Instruction* a, spvgentwo::Instruction* b, spvgentwo::Instruction*& outA, spvgentwo::Instruction*& outB)
	{
		spvgentwo::BasicBlock& bb = *(*m_func);

		auto aType = a->getType();
		auto bType = b->getType();
		auto aBaseType = aType->getBaseType();
		auto bBaseType = bType->getBaseType();

		outA = a;
		outB = b;

		if (aBaseType.isFloat() && !bBaseType.isFloat())
			outB = m_simpleConvert(expr::TokenType_Float, outB);
		else if (bBaseType.isFloat() && !aBaseType.isFloat())
			outA = m_simpleConvert(expr::TokenType_Float, outA);

		if (op != '*' || (op == '*' && !bBaseType.isFloat() && !aBaseType.isFloat())) {
			if (aType->isVector() && !bType->isVector()) {
				switch (aType->getVectorComponentCount()) {
				case 2: outB = bb->opCompositeConstruct(a->getTypeInstr(), outB, outB); break;
				case 3: outB = bb->opCompositeConstruct(a->getTypeInstr(), outB, outB, outB); break;
				case 4: outB = bb->opCompositeConstruct(a->getTypeInstr(), outB, outB, outB, outB); break;
				}
			} else if (bType->isVector() && !aType->isVector()) {
				switch (aType->getVectorComponentCount()) {
				case 2: outA = bb->opCompositeConstruct(a->getTypeInstr(), outA, outA); break;
				case 3: outA = bb->opCompositeConstruct(a->getTypeInstr(), outA, outA, outA); break;
				case 4: outA = bb->opCompositeConstruct(a->getTypeInstr(), outA, outA, outA, outA); break;
				}
			}
		}
	}
	spvgentwo::Instruction* ExpressionCompiler::m_simpleConvert(int type, spvgentwo::Instruction* inst)
	{
		spvgentwo::BasicBlock& bb = *(*m_func);

		if (type == expr::TokenType_Int) {
			if (inst->getType()->getBaseType().isFloat())
				return bb->opConvertFToS(inst);
		} else if (type == expr::TokenType_Uint) {
			if (inst->getType()->getBaseType().isFloat())
				return bb->opConvertFToU(inst);
		} else if (type == expr::TokenType_Float) {
			if (inst->getType()->getBaseType().isSInt())
				return bb->opConvertSToF(inst);
			else if (inst->getType()->getBaseType().isUInt())
				return bb->opConvertUToF(inst);
		}

		return inst;
	}
	int ExpressionCompiler::m_getBaseType(int type)
	{
		switch (type) {
		case expr::TokenType_Int:
		case expr::TokenType_Int2:
		case expr::TokenType_Int3:
		case expr::TokenType_Int4:
			return expr::TokenType_Int;
		case expr::TokenType_Uint:
		case expr::TokenType_Uint2:
		case expr::TokenType_Uint3:
		case expr::TokenType_Uint4:
			return expr::TokenType_Uint;
		case expr::TokenType_Bool:
		case expr::TokenType_Bool2:
		case expr::TokenType_Bool3:
		case expr::TokenType_Bool4:
			return expr::TokenType_Bool;
		default: break;
		}

		return expr::TokenType_Float;
	}
	int ExpressionCompiler::m_getCompCount(int type)
	{
		switch (type) {
		case expr::TokenType_Float4:
		case expr::TokenType_Bool4:
		case expr::TokenType_Int4:
		case expr::TokenType_Uint4:
			return 4;
		case expr::TokenType_Float3:
		case expr::TokenType_Bool3:
		case expr::TokenType_Int3:
		case expr::TokenType_Uint3:
			return 3;
		case expr::TokenType_Float2:
		case expr::TokenType_Bool2:
		case expr::TokenType_Int2:
		case expr::TokenType_Uint2:
			return 2;
		default: break;
		}

		return 4;
	}
	bool ExpressionCompiler::m_isVector(int type)
	{
		switch (type) {
		case expr::TokenType_Float4:
		case expr::TokenType_Bool4:
		case expr::TokenType_Int4:
		case expr::TokenType_Uint4:
		case expr::TokenType_Float3:
		case expr::TokenType_Bool3:
		case expr::TokenType_Int3:
		case expr::TokenType_Uint3:
		case expr::TokenType_Float2:
		case expr::TokenType_Bool2:
		case expr::TokenType_Int2:
		case expr::TokenType_Uint2:
			return true;
		}

		return false;
	}
	spvgentwo::Instruction* ExpressionCompiler::m_constructVector(int baseType, int compCount, std::vector<spvgentwo::Instruction*>& comps)
	{
		spvgentwo::Instruction* baseTypeInstr = nullptr;

		if (compCount == 2) {
			if (baseType == expr::TokenType_Float)
				baseTypeInstr = m_module->type<spvgentwo::vector_t<float, 2>>();
			else if (baseType == expr::TokenType_Int)
				baseTypeInstr = m_module->type<spvgentwo::vector_t<int, 2>>();
			else if (baseType == expr::TokenType_Uint)
				baseTypeInstr = m_module->type<spvgentwo::vector_t<unsigned int, 2>>();
			else if (baseType == expr::TokenType_Bool)
				baseTypeInstr = m_module->type<spvgentwo::vector_t<bool, 2>>();
		} else if (compCount == 3) {
			if (baseType == expr::TokenType_Float)
				baseTypeInstr = m_module->type<spvgentwo::vector_t<float, 3>>();
			else if (baseType == expr::TokenType_Int)
				baseTypeInstr = m_module->type<spvgentwo::vector_t<int, 3>>();
			else if (baseType == expr::TokenType_Uint)
				baseTypeInstr = m_module->type<spvgentwo::vector_t<unsigned int, 3>>();
			else if (baseType == expr::TokenType_Bool)
				baseTypeInstr = m_module->type<spvgentwo::vector_t<bool, 3>>();
		} else if (compCount == 4) {
			if (baseType == expr::TokenType_Float)
				baseTypeInstr = m_module->type<spvgentwo::vector_t<float, 4>>();
			else if (baseType == expr::TokenType_Int)
				baseTypeInstr = m_module->type<spvgentwo::vector_t<int, 4>>();
			else if (baseType == expr::TokenType_Uint)
				baseTypeInstr = m_module->type<spvgentwo::vector_t<unsigned int, 4>>();
			else if (baseType == expr::TokenType_Bool)
				baseTypeInstr = m_module->type<spvgentwo::vector_t<bool, 4>>();
		}

		spvgentwo::BasicBlock& bb = *(*m_func);

		spvgentwo::Instruction* args[4] = { nullptr };
		int curArg = 0;
		for (int i = 0; i < comps.size(); i++) {
			if (comps[i]->getType()->isScalar()) {
				args[curArg] = m_simpleConvert(baseType, comps[i]);
				curArg++;
			} else if (comps[i]->getType()->isVector()) {
				for (int j = 0; j < comps[i]->getType()->getVectorComponentCount(); j++) {
					args[curArg] = m_simpleConvert(baseType, bb->opCompositeExtract(comps[i], j));
					curArg++;
				}
			}
		}
		for (int i = 1; i < 4; i++)
			if (args[i] == nullptr)
				args[i] = args[i - 1];

		if (compCount == 2)
			return bb->opCompositeConstruct(baseTypeInstr, args[0], args[1]);
		else if (compCount == 3)
			return bb->opCompositeConstruct(baseTypeInstr, args[0], args[1], args[2]);
		else if (compCount == 4)
			return bb->opCompositeConstruct(baseTypeInstr, args[0], args[1], args[2], args[3]);

		return nullptr;
	}
	spvgentwo::Instruction* ExpressionCompiler::m_swizzle(spvgentwo::Instruction* vec, const char* field)
	{
		spvgentwo::BasicBlock& bb = *(*m_func);
		const std::vector<const char*> combo = {
			"xyzw",
			"rgba",
			"stpq"
		};

		if (strlen(field) == 1) {
			for (int i = 0; i < combo.size(); i++)
				for (int j = 0; j < 4; j++)
					if (combo[i][j] == field[0])
						return bb->opCompositeExtract(vec, j);
		} else {
			std::vector<spvgentwo::Instruction*> comps;
			for (int i = 0; i < strlen(field); i++)
				for (int j = 0; j < combo.size(); j++)
					for (int k = 0; k < 4; k++)
						if (combo[j][k] == field[i]) {
							comps.push_back(bb->opCompositeExtract(vec, k));
							break;
						}

			int baseType = expr::TokenType_Float;
			if (vec->getType()->getBaseType().isSInt())
				baseType = expr::TokenType_Int;
			else if (vec->getType()->getBaseType().isUInt())
				baseType = expr::TokenType_Uint;
			else if (vec->getType()->getBaseType().isBool())
				baseType = expr::TokenType_Bool;

			return m_constructVector(baseType, comps.size(), comps);
		}

		return nullptr;
	}
}