#include "ASTNode.h"

#include "API/LLVM/LLVMBackend.h"
#include "Core/Log.h"
#include "Core/Types.h"

#include <iostream>
#include <map>
#include <stack>

namespace clear {

	static std::map<std::string, llvm::AllocaInst*>     s_VariableMap;
	static std::map<std::string, ObjectReferenceInfo>   s_StructTypes;
	static std::stack<llvm::IRBuilderBase::InsertPoint> s_InsertPoints;
	static std::map<std::string, std::vector<Paramater>> s_FunctionToExpectedTypes;

	llvm::Value* ASTNodeBase::Codegen()
	{


		for (auto& child : GetChildren())
			child->Codegen();

		return nullptr;
	}

	void ASTNodeBase::PushChild(const Ref<ASTNodeBase>& child)
	{
		m_Children.push_back(child);
	}
	void ASTNodeBase::RemoveChild(const Ref<ASTNodeBase>& child)
	{
		auto it = std::find(m_Children.begin(), m_Children.end(), child);
		if (it != m_Children.end())
			m_Children.erase(it);
	}
	void ASTNodeBase::SetParent(const Ref<ASTNodeBase>& parent)
	{
		m_Parent = parent;
	}
	void ASTNodeBase::RemoveParent()
	{
		m_Parent.Reset();
	}
	ASTNodeLiteral::ASTNodeLiteral(const std::string& data)
		: m_Data(data), m_Type(data)
	{
	}

	llvm::Value* ASTNodeLiteral::Codegen()
	{
		return GetLLVMConstant(m_Type, m_Data);
	}

	ASTBinaryExpression::ASTBinaryExpression(BinaryExpressionType type, AbstractType expectedType)
		: m_Expression(type), m_ExpectedType(expectedType)
	{
	}
	llvm::Value* ASTBinaryExpression::Codegen()
	{
		// Assumes the two values in its children are to be added in order
		auto& builder = *LLVM::Backend::GetBuilder();
		auto& children = GetChildren();

		if (children.size() != 2)
			return nullptr;

		llvm::Value* LHS = children[1]->Codegen();
		llvm::Value* RHS = children[0]->Codegen();

		CLEAR_VERIFY(LHS && RHS, "lhs or rhs failed to generate");

		llvm::Value* LHSRawValue = LHS;
		llvm::Value* RHSRawValue = RHS;

		if (llvm::isa<llvm::AllocaInst>(RHS))
		{
			auto converted = llvm::dyn_cast<llvm::AllocaInst>(RHS);
			RHSRawValue = builder.CreateLoad(converted->getAllocatedType(), RHS);
		}

		// Load values if they are alloca instructions
		if (llvm::isa<llvm::AllocaInst>(LHS) && m_Expression != BinaryExpressionType::Assignment)
		{
			auto converted = llvm::dyn_cast<llvm::AllocaInst>(LHS);
			LHSRawValue = builder.CreateLoad(converted->getAllocatedType(), LHS);
		}
		else if (llvm::isa<llvm::AllocaInst>(LHS) && m_Expression == BinaryExpressionType::Assignment)
		{
			return _CreateExpression(LHS, RHS, LHSRawValue, RHSRawValue);
		}

		llvm::Type* expectedLLVMType = m_ExpectedType.GetLLVMType(); 

		switch (m_ExpectedType.Get())
		{
			case VariableType::Int8:
			case VariableType::Int16:
			case VariableType::Int32:
			case VariableType::Int64:
			case VariableType::Uint8:
			case VariableType::Uint16:
			case VariableType::Uint32:
			case VariableType::Uint64:
			{
				if (LHSRawValue->getType() != expectedLLVMType)
					LHSRawValue = AbstractType::CastValue(LHSRawValue, m_ExpectedType);

				if (RHSRawValue->getType() != expectedLLVMType)
					RHSRawValue = AbstractType::CastValue(RHSRawValue, m_ExpectedType);
				break;
			}
			case VariableType::Float32:
			case VariableType::Float64:
			{
				if (LHSRawValue->getType() != expectedLLVMType)
					LHSRawValue = AbstractType::CastValue(LHSRawValue, m_ExpectedType);

				if (RHSRawValue->getType() != expectedLLVMType)
					RHSRawValue = AbstractType::CastValue(RHSRawValue, m_ExpectedType);
				break;
			}
			case VariableType::Bool:
			{
				if (LHSRawValue->getType() != expectedLLVMType)
					LHSRawValue = AbstractType::CastValue(LHSRawValue, m_ExpectedType);

				if (RHSRawValue->getType() != expectedLLVMType)
					RHSRawValue = AbstractType::CastValue(RHSRawValue, m_ExpectedType);

				break;
			}
			case VariableType::None:
			default:
				//no special handling just assume the types are compatible
				break;
		}

		return _CreateExpression(LHS, RHS, LHSRawValue, RHSRawValue);
	}

	const bool ASTBinaryExpression::_IsMathExpression() const
	{
		return (int)m_Expression <= (int)BinaryExpressionType::Mod;
	}

	const bool ASTBinaryExpression::_IsCmpExpression() const
	{
		return (int)m_Expression <= (int)BinaryExpressionType::Eq && !_IsMathExpression();
	}

	llvm::Value* ASTBinaryExpression::_CreateExpression(llvm::Value* LHS, llvm::Value* RHS, llvm::Value* LHSRawValue, llvm::Value* RHSRawValue)
	{
		if (_IsMathExpression())
			return _CreateMathExpression(LHSRawValue, RHSRawValue);
		else if (_IsCmpExpression())
			return _CreateCmpExpression(LHSRawValue, RHSRawValue);
		
		return _CreateLoadStoreExpression(LHS, RHSRawValue);
	}

	llvm::Value* ASTBinaryExpression::_CreateMathExpression(llvm::Value* LHS, llvm::Value* RHS)
	{
		auto& builder = LLVM::Backend::GetBuilder();
		const bool isFloat = LHS->getType()->isFloatingPointTy();

		switch (m_Expression)
		{
			case BinaryExpressionType::Add:
				return isFloat ? builder->CreateFAdd(LHS, RHS, "faddtmp")
							   : builder->CreateAdd(LHS, RHS,  "addtmp");

			case BinaryExpressionType::Sub:
				return isFloat ? builder->CreateFSub(LHS, RHS, "fsubtmp")
							   : builder->CreateSub(LHS, RHS,  "subtmp");

			case BinaryExpressionType::Mul:
				return isFloat ? builder->CreateFMul(LHS, RHS, "fmultmp")
							   : builder->CreateMul(LHS, RHS,  "multmp");

			case BinaryExpressionType::Div:
				return isFloat ? builder->CreateFDiv(LHS, RHS, "fdivtmp")
							   : builder->CreateSDiv(LHS, RHS, "divtmp");

			case BinaryExpressionType::Mod:
				if (!isFloat)
					return builder->CreateSRem(LHS, RHS, "modtmp");

				break;
			default:
				break;
		}

		return nullptr;
	}
	llvm::Value* ASTBinaryExpression::_CreateCmpExpression(llvm::Value* LHS, llvm::Value* RHS)
	{
		auto& builder = LLVM::Backend::GetBuilder();
		const bool isFloat = LHS->getType()->isFloatingPointTy();

		switch (m_Expression)
		{
			case BinaryExpressionType::Less:
				return isFloat ? builder->CreateFCmpOLT(LHS, RHS)
							   : builder->CreateICmpSLT(LHS, RHS);
			case BinaryExpressionType::LessEq:
				return isFloat ? builder->CreateFCmpOLE(LHS, RHS)
							   : builder->CreateICmpSLE(LHS, RHS);
			case BinaryExpressionType::Greater:
				return isFloat ? builder->CreateFCmpOGT(LHS, RHS)
							   : builder->CreateICmpSGT(LHS, RHS);
			case BinaryExpressionType::GreaterEq:
				return isFloat ? builder->CreateFCmpOGE(LHS, RHS)
							   : builder->CreateICmpSGE(LHS, RHS);
			case BinaryExpressionType::Eq:
				return isFloat ? builder->CreateFCmpOEQ(LHS, RHS)
							   : builder->CreateICmpEQ(LHS, RHS);
			default:
				break;
		}

		return nullptr;
	}

	llvm::Value* ASTBinaryExpression::_CreateLoadStoreExpression(llvm::Value* LHS, llvm::Value* RHS)
	{
		auto& builder = LLVM::Backend::GetBuilder();

		switch (m_Expression)
		{
			case BinaryExpressionType::Assignment:	return builder->CreateStore(RHS, LHS);
				default:
					break;
		}

		return nullptr;
	}

	ASTVariableExpression::ASTVariableExpression(const std::string& name)
		: m_Name(name)
	{
	}

	llvm::Value* ASTVariableExpression::Codegen()
	{
		if (!s_VariableMap.contains(m_Name))
		{
			std::cout << "no variable of name " << m_Name << " exists" << std::endl;
			return nullptr;
		}

		llvm::AllocaInst* value = s_VariableMap.at(m_Name);
		CLEAR_VERIFY(value, "value was nullptr");

		return value;
	}

	ASTVariableDecleration::ASTVariableDecleration(const std::string& name, AbstractType type)
		: m_Name(name), m_Type(type)
	{
	}

	llvm::Value* ASTVariableDecleration::Codegen()
	{
		if (s_VariableMap.contains(m_Name))
		{
			std::cout << "variable of the name " << m_Name << " exists" << std::endl;
			return nullptr;
		}

		auto& builder = *LLVM::Backend::GetBuilder();

		if (m_Type.Get() == VariableType::UserDefinedType)
		{
			auto& structType = m_Type.GetUserDefinedType();

			auto value = builder.CreateAlloca(s_StructTypes[structType].Struct, nullptr, m_Name);
			s_VariableMap[m_Name] = value;

			return value;
		}

		auto variableType = m_Type.Get();
		auto value = builder.CreateAlloca(GetLLVMVariableType(variableType), nullptr, m_Name);
		s_VariableMap[m_Name] = value;
		
		return value;
	}

	ASTFunctionDecleration::ASTFunctionDecleration(const std::string& name, VariableType returnType, const std::vector<Paramater>& Paramaters)
		: m_Name(name), m_ReturnType(returnType), m_Paramaters(Paramaters)
	{
		s_FunctionToExpectedTypes[m_Name] = m_Paramaters;

		if (!s_FunctionToExpectedTypes.contains("_sleep")) //TODO: remove thhis immediately
		{
			auto& e = s_FunctionToExpectedTypes["_sleep"];
			e.push_back({ .Name = "time", .Type = AbstractType(VariableType::Int32) });
		}

	}
	llvm::Value* ASTFunctionDecleration::Codegen()
	{
		auto& module  = *LLVM::Backend::GetModule();
		auto& context = *LLVM::Backend::GetContext();
		auto& builder = *LLVM::Backend::GetBuilder();

		s_InsertPoints.push(builder.saveIP());

		llvm::Type* returnType = GetLLVMVariableType(m_ReturnType);

		std::vector<llvm::Type*> ParamaterTypes;
		for (const auto& Paramater : m_Paramaters)
		{
			ParamaterTypes.push_back(GetLLVMVariableType(Paramater.Type));
		}

		llvm::FunctionType* functionType = llvm::FunctionType::get(returnType, ParamaterTypes, false);

		llvm::Function* function = module.getFunction(m_Name);
		CLEAR_VERIFY(!function, "function already defined");

		function = llvm::Function::Create(functionType, llvm::Function::ExternalLinkage, m_Name, module);

		llvm::Function::arg_iterator args = function->arg_begin();

		for (const auto& Paramater : m_Paramaters)
		{
			args->setName(Paramater.Name);
			args++;
		}

		llvm::BasicBlock* entry = llvm::BasicBlock::Create(context, "entry", function);
		builder.SetInsertPoint(entry);

		uint32_t k = 0;
		for (const auto& Paramater : m_Paramaters)
		{
			llvm::AllocaInst* argAlloc = builder.CreateAlloca(GetLLVMVariableType(Paramater.Type), nullptr, Paramater.Name);
			builder.CreateStore(function->getArg(k), argAlloc);
			s_VariableMap[m_Name + "::" + Paramater.Name] = argAlloc;
			k++;
		}

		for (const auto& child : GetChildren())
		{
			child->Codegen();

			if (child->GetType() == ASTNodeType::ReturnStatement)
				break;
		}

		for (const auto& Paramater : m_Paramaters)
		{
			s_VariableMap.erase(m_Name + "::" + Paramater.Name);
		}

		if (returnType->isVoidTy() && !builder.GetInsertBlock()->getTerminator())
		{
			builder.CreateRetVoid();
		}

		auto& ip = s_InsertPoints.top();
		builder.restoreIP(ip);
		s_InsertPoints.pop();

		return function;
	}

	llvm::Value* ASTReturnStatement::Codegen()
	{
		auto& builder = *LLVM::Backend::GetBuilder();

		if (GetChildren().size() > 0)
		{
			llvm::Value* returnValue = GetChildren()[0]->Codegen();
			return builder.CreateRet(returnValue);
		}

		return builder.CreateRetVoid();
	}
	llvm::Value* ASTExpression::Codegen()
	{
		auto& builder  = *LLVM::Backend::GetBuilder();
		auto& children = GetChildren();

		std::stack<Ref<ASTNodeBase>> stack;

		for (const auto& child : children)
		{
			if (child->GetType() == ASTNodeType::Literal ||
				child->GetType() == ASTNodeType::VariableExpression)
			{
				stack.push(child);
				continue;
			}

			Ref<ASTBinaryExpression> binExp = ASTBinaryExpression::DynamicCast<ASTBinaryExpression>(child);

			binExp->PushChild(stack.top());
			stack.pop();

			binExp->PushChild(stack.top());
			stack.pop();

			stack.push(binExp);
		}

		return stack.top()->Codegen();
	}

	ASTStruct::ASTStruct(const std::string& name, const std::vector<Member>& fields)
		: m_Name(name), m_Members(fields)
	{
	}

	llvm::Value* ASTStruct::Codegen()
	{
		std::vector<llvm::Type*> types;

		ObjectReferenceInfo info;
		uint32_t k = 0;

		for (auto& member : m_Members)
		{
			if (member.Field.Get() == VariableType::UserDefinedType)
			{
				auto& structName = member.Field.GetUserDefinedType();

				CLEAR_VERIFY(s_StructTypes.contains(structName), "struct hasn't been declared");
				types.push_back(s_StructTypes[structName].Struct);
			}
			else
			{
				auto& variableType = member.Field;
				types.push_back(GetLLVMVariableType(variableType));
			}

			info.Indices[member.Name] = k++;
		}

		info.Struct = llvm::StructType::create(types);
		s_StructTypes[m_Name] = info;

		return nullptr;
	}

	ASTFunctionCall::ASTFunctionCall(const std::string& name, const std::vector<Argument>& arguments)
		:  m_Name(name), m_Arguments(arguments)
	{
	}

	llvm::Value* ASTFunctionCall::Codegen()
	{
		std::vector<llvm::Value*> args;

		auto& builder = *LLVM::Backend::GetBuilder();
		auto& module  = *LLVM::Backend::GetModule();

		auto& expected = s_FunctionToExpectedTypes.at(m_Name);

		uint32_t k = 0;
		for (const auto& argument : m_Arguments)
		{
			if(argument.Field.GetKind() == TypeKind::RValue)
			{
				auto& variableType = argument.Field;
				auto value = GetLLVMConstant(variableType, argument.Data);

				if (variableType.Get() != expected[k].Type.Get())
					value = AbstractType::CastValue(value, expected[k].Type);

				args.push_back(value);
			}
			else
			{
				auto& variableName = argument.Data;
				auto& variableType = argument.Field;
				auto& variable = s_VariableMap.at(variableName);

				llvm::Value* value = builder.CreateLoad(variable->getAllocatedType(), variable);

				if (variableType.Get() != expected[k].Type.Get())
					value = AbstractType::CastValue(value, expected[k].Type);

				args.push_back(value);
			}
			k++;
		}


		if (m_Name == "_sleep") 
		{
			llvm::Function* sleepFunc = llvm::cast<llvm::Function>(
				module.getOrInsertFunction("_sleep",
					llvm::FunctionType::get(llvm::Type::getInt32Ty(module.getContext()),
						llvm::Type::getInt32Ty(module.getContext()),
						false)).getCallee());
		}
		else if (m_Name == "sleep")
		{
			llvm::Function* sleepFunc = llvm::cast<llvm::Function>(
				module.getOrInsertFunction("sleep",
					llvm::FunctionType::get(llvm::Type::getInt32Ty(module.getContext()),
						llvm::Type::getInt32Ty(module.getContext()),
						false)).getCallee());
		}
		else if (m_Name == "nanosleep")
		{
			llvm::Function* sleepFunc = llvm::cast<llvm::Function>(
				module.getOrInsertFunction("nanosleep",
					llvm::FunctionType::get(llvm::Type::getInt32Ty(module.getContext()),
						llvm::Type::getInt32Ty(module.getContext()),
						false)).getCallee());
		}
		

		llvm::Function* callee = module.getFunction(m_Name);
		CLEAR_VERIFY(callee, "not a valid function");

		return builder.CreateCall(callee, args);
	}
}