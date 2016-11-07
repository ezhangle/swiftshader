// Copyright 2016 The SwiftShader Authors. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//    http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "Nucleus.hpp"

#include "Reactor.hpp"
#include "Routine.hpp"

#include "src/IceTypes.h"
#include "src/IceCfg.h"
#include "src/IceELFStreamer.h"
#include "src/IceGlobalContext.h"
#include "src/IceCfgNode.h"
#include "src/IceELFObjectWriter.h"
#include "src/IceGlobalInits.h"

#include "llvm/Support/FileSystem.h"
#include "llvm/Support/raw_os_ostream.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include <mutex>
#include <limits>
#include <iostream>
#include <cassert>

namespace
{
	Ice::GlobalContext *context = nullptr;
	Ice::Cfg *function = nullptr;
	Ice::CfgNode *basicBlock = nullptr;
	Ice::CfgLocalAllocatorScope *allocator = nullptr;
	sw::Routine *routine = nullptr;

	std::mutex codegenMutex;

	sw::BasicBlock *falseBB = nullptr;

	Ice::ELFFileStreamer *elfFile = nullptr;
	Ice::Fdstream *out = nullptr;
}

namespace sw
{
	enum EmulatedType
	{
		EmulatedShift = 16,
		EmulatedV2 = 2 << EmulatedShift,
		EmulatedV4 = 4 << EmulatedShift,
		EmulatedV8 = 8 << EmulatedShift,
		EmulatedBits = EmulatedV2 | EmulatedV4 | EmulatedV8,

		Type_v2i32 = Ice::IceType_v4i32 | EmulatedV2,
		Type_v4i16 = Ice::IceType_v8i16 | EmulatedV4,
		Type_v2i16 = Ice::IceType_v8i16 | EmulatedV2,
		Type_v8i8 =  Ice::IceType_v16i8 | EmulatedV8,
		Type_v4i8 =  Ice::IceType_v16i8 | EmulatedV4,
		Type_v2f32 = Ice::IceType_v4f32 | EmulatedV2,
	};

	class Value : public Ice::Variable {};
	class BasicBlock : public Ice::CfgNode {};

	Ice::Type T(Type *t)
	{
		static_assert(Ice::IceType_NUM < EmulatedBits, "Ice::Type overlaps with our emulated types!");
		return (Ice::Type)(reinterpret_cast<std::intptr_t>(t) & ~EmulatedBits);
	}

	Type *T(Ice::Type t)
	{
		return reinterpret_cast<Type*>(t);
	}

	Type *T(EmulatedType t)
	{
		return reinterpret_cast<Type*>(t);
	}

	Value *V(Ice::Variable *v)
	{
		return reinterpret_cast<Value*>(v);
	}

	Value *C(Ice::Constant *c)   // Only safe for casting right-hand side operand
	{
		return reinterpret_cast<Value*>(c);
	}

	BasicBlock *B(Ice::CfgNode *b)
	{
		return reinterpret_cast<BasicBlock*>(b);
	}

	Optimization optimization[10] = {InstructionCombining, Disabled};

	using ElfHeader = std::conditional<sizeof(void*) == 8, Elf64_Ehdr, Elf32_Ehdr>::type;
	using SectionHeader = std::conditional<sizeof(void*) == 8, Elf64_Shdr, Elf32_Shdr>::type;

	inline const SectionHeader *sectionHeader(const ElfHeader *elfHeader)
	{
		return reinterpret_cast<const SectionHeader*>((intptr_t)elfHeader + elfHeader->e_shoff);
	}
 
	inline const SectionHeader *elfSection(const ElfHeader *elfHeader, int index)
	{
		return &sectionHeader(elfHeader)[index];
	}

	static void *relocateSymbol(const ElfHeader *elfHeader, const Elf32_Rel &relocation, const SectionHeader &relocationTable)
	{
		const SectionHeader *target = elfSection(elfHeader, relocationTable.sh_info);
 
		intptr_t address = (intptr_t)elfHeader + target->sh_offset;
		int32_t *patchSite = (int*)(address + relocation.r_offset);
		uint32_t index = relocation.getSymbol();
		int table = relocationTable.sh_link;
		void *symbolValue = nullptr;
		
		if(index != SHN_UNDEF)
		{
			if(table == SHN_UNDEF) return nullptr;
			const SectionHeader *symbolTable = elfSection(elfHeader, table);
 
			uint32_t symtab_entries = symbolTable->sh_size / symbolTable->sh_entsize;
			if(index >= symtab_entries)
			{
				assert(index < symtab_entries && "Symbol Index out of range");
				return nullptr;
			}
 
			intptr_t symbolAddress = (intptr_t)elfHeader + symbolTable->sh_offset;
			Elf32_Sym &symbol = ((Elf32_Sym*)symbolAddress)[index];
			uint16_t section = symbol.st_shndx;

			if(section != SHN_UNDEF && section < SHN_LORESERVE)
			{
				const SectionHeader *target = elfSection(elfHeader, symbol.st_shndx);
				symbolValue = reinterpret_cast<void*>((intptr_t)elfHeader + symbol.st_value + target->sh_offset);
			}
			else
			{
				return nullptr;
			}
		}

		switch(relocation.getType())
		{
		case R_386_NONE:
			// No relocation
			break;
		case R_386_32:
			*patchSite = (int32_t)((intptr_t)symbolValue + *patchSite);
			break;
	//	case R_386_PC32:
	//		*patchSite = (int32_t)((intptr_t)symbolValue + *patchSite - (intptr_t)patchSite);
	//		break;
		default:
			assert(false && "Unsupported relocation type");
			return nullptr;
		}

		return symbolValue;
	}

	static void *relocateSymbol(const ElfHeader *elfHeader, const Elf64_Rela &relocation, const SectionHeader &relocationTable)
	{
		const SectionHeader *target = elfSection(elfHeader, relocationTable.sh_info);
 
		intptr_t address = (intptr_t)elfHeader + target->sh_offset;
		int32_t *patchSite = (int*)(address + relocation.r_offset);
		uint32_t index = relocation.getSymbol();
		int table = relocationTable.sh_link;
		void *symbolValue = nullptr;

		if(index != SHN_UNDEF)
		{
			if(table == SHN_UNDEF) return nullptr;
			const SectionHeader *symbolTable = elfSection(elfHeader, table);
 
			uint32_t symtab_entries = symbolTable->sh_size / symbolTable->sh_entsize;
			if(index >= symtab_entries)
			{
				assert(index < symtab_entries && "Symbol Index out of range");
				return nullptr;
			}
 
			intptr_t symbolAddress = (intptr_t)elfHeader + symbolTable->sh_offset;
			Elf64_Sym &symbol = ((Elf64_Sym*)symbolAddress)[index];
			uint16_t section = symbol.st_shndx;

			if(section != SHN_UNDEF && section < SHN_LORESERVE)
			{
				const SectionHeader *target = elfSection(elfHeader, symbol.st_shndx);
				symbolValue = reinterpret_cast<void*>((intptr_t)elfHeader + symbol.st_value + target->sh_offset);
			}
			else
			{
				return nullptr;
			}
		}

		switch(relocation.getType())
		{
		case R_X86_64_NONE:
			// No relocation
			break;
	//	case R_X86_64_64:
	//		*patchSite = (int32_t)((intptr_t)symbolValue + *patchSite) + relocation->r_addend;
	//		break;
		case R_X86_64_PC32:
			*patchSite = (int32_t)((intptr_t)symbolValue + *patchSite - (intptr_t)patchSite) + relocation.r_addend;
			break;
	//	case R_X86_64_32S:
	//		*patchSite = (int32_t)((intptr_t)symbolValue + *patchSite) + relocation.r_addend;
	//		break;
		default:
			assert(false && "Unsupported relocation type");
			return nullptr;
		}

		return symbolValue;
	}

	void *loadImage(uint8_t *const elfImage)
	{
		ElfHeader *elfHeader = (ElfHeader*)elfImage;

		if(!elfHeader->checkMagic())
		{
			return nullptr;
		}

		// Expect ELF bitness to match platform
		assert(sizeof(void*) == 8 ? elfHeader->getFileClass() == ELFCLASS64 : elfHeader->getFileClass() == ELFCLASS32);
		assert(sizeof(void*) == 8 ? elfHeader->e_machine == EM_X86_64 : elfHeader->e_machine == EM_386);

		SectionHeader *sectionHeader = (SectionHeader*)(elfImage + elfHeader->e_shoff);
		void *entry = nullptr;

		for(int i = 0; i < elfHeader->e_shnum; i++)
		{
			if(sectionHeader[i].sh_type == SHT_PROGBITS)
			{
				if(sectionHeader[i].sh_flags & SHF_EXECINSTR)
				{
					entry = elfImage + sectionHeader[i].sh_offset;
				}
			}
			else if(sectionHeader[i].sh_type == SHT_REL)
			{
				assert(sizeof(void*) == 4 && "UNIMPLEMENTED");   // Only expected/implemented for 32-bit code

				for(int index = 0; index < sectionHeader[i].sh_size / sectionHeader[i].sh_entsize; index++)
				{
					const Elf32_Rel &relocation = ((const Elf32_Rel*)(elfImage + sectionHeader[i].sh_offset))[index];
					void *symbol = relocateSymbol(elfHeader, relocation, sectionHeader[i]);
				}
			}
			else if(sectionHeader[i].sh_type == SHT_RELA)
			{
				assert(sizeof(void*) == 8 && "UNIMPLEMENTED");   // Only expected/implemented for 64-bit code

				for(int index = 0; index < sectionHeader[i].sh_size / sectionHeader[i].sh_entsize; index++)
				{
					const Elf64_Rela &relocation = ((const Elf64_Rela*)(elfImage + sectionHeader[i].sh_offset))[index];
					void *symbol = relocateSymbol(elfHeader, relocation, sectionHeader[i]);
				}
			}
		}

		return entry;
	}

	template<typename T>
	struct ExecutableAllocator
	{
		ExecutableAllocator() {};
		template<class U> ExecutableAllocator(const ExecutableAllocator<U> &other) {};

		using value_type = T;
		using size_type = std::size_t;

		T *allocate(size_type n)
		{
			return (T*)VirtualAlloc(NULL, sizeof(T) * n, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
		}

		void deallocate(T *p, size_type n)
		{
			VirtualFree(p, 0, MEM_RELEASE);
		}
	};

	class ELFMemoryStreamer : public Ice::ELFStreamer, public Routine
	{
		ELFMemoryStreamer(const ELFMemoryStreamer &) = delete;
		ELFMemoryStreamer &operator=(const ELFMemoryStreamer &) = delete;

	public:
		ELFMemoryStreamer() : Routine(), entry(nullptr)
		{
			position = 0;
			buffer.reserve(0x1000);
		}

		virtual ~ELFMemoryStreamer()
		{
			if(buffer.size() != 0)
			{
				DWORD exeProtection;
				VirtualProtect(&buffer[0], buffer.size(), oldProtection, &exeProtection);
			}
		}

		void write8(uint8_t Value) override
		{
			if(position == (uint64_t)buffer.size())
			{
				buffer.push_back(Value);
				position++;
			}
			else if(position < (uint64_t)buffer.size())
			{
				buffer[position] = Value;
				position++;
			}
			else assert(false && "UNIMPLEMENTED");
		}

		void writeBytes(llvm::StringRef Bytes) override
		{
			std::size_t oldSize = buffer.size();
			buffer.resize(oldSize + Bytes.size());
			memcpy(&buffer[oldSize], Bytes.begin(), Bytes.size());
			position += Bytes.size();
		}

		uint64_t tell() const override { return position; }

		void seek(uint64_t Off) override { position = Off; }

		const void *getEntry() override
		{
			if(!entry)
			{
				VirtualProtect(&buffer[0], buffer.size(), PAGE_EXECUTE_READWRITE, &oldProtection);
				position = std::numeric_limits<std::size_t>::max();  // Can't stream more data after this

				entry = loadImage(&buffer[0]);
			}

			return entry;
		}

	private:
		void *entry;
		std::vector<uint8_t, ExecutableAllocator<uint8_t>> buffer;
		std::size_t position;
		DWORD oldProtection;
	};

	Nucleus::Nucleus()
	{
		::codegenMutex.lock();   // Reactor is currently not thread safe

		Ice::ClFlags &Flags = Ice::ClFlags::Flags;
		Ice::ClFlags::getParsedClFlags(Flags);

		Flags.setTargetArch(sizeof(void*) == 8 ? Ice::Target_X8664 : Ice::Target_X8632);
		Flags.setOutFileType(Ice::FT_Elf);
		Flags.setOptLevel(Ice::Opt_2);
		Flags.setApplicationBinaryInterface(Ice::ABI_Platform);
		Flags.setVerbose(false ? Ice::IceV_All : Ice::IceV_None);

		static llvm::raw_os_ostream cout(std::cout);
		static llvm::raw_os_ostream cerr(std::cerr);

		if(false)   // Write out to a file
		{
			std::error_code errorCode;
			::out = new Ice::Fdstream("out.o", errorCode, llvm::sys::fs::F_None);
			::elfFile = new Ice::ELFFileStreamer(*out);
			::context = new Ice::GlobalContext(&cout, &cout, &cerr, elfFile);
		}
		else
		{
			ELFMemoryStreamer *elfMemory = new ELFMemoryStreamer();
			::context = new Ice::GlobalContext(&cout, &cout, &cerr, elfMemory);
			::routine = elfMemory;
		}
	}

	Nucleus::~Nucleus()
	{
		delete ::allocator;
		delete ::function;
		delete ::context;

		delete ::elfFile;
		delete ::out;

		::codegenMutex.unlock();
	}

	Routine *Nucleus::acquireRoutine(const wchar_t *name, bool runOptimizations)
	{
		if(basicBlock->getInsts().empty() || basicBlock->getInsts().back().getKind() != Ice::Inst::Ret)
		{
			createRetVoid();
		}

		std::wstring wideName(name);
		std::string asciiName(wideName.begin(), wideName.end());
		::function->setFunctionName(Ice::GlobalString::createWithString(::context, asciiName));

		::function->translate();
		assert(!::function->hasError());

		auto *globals = ::function->getGlobalInits().release();

		if(globals && !globals->empty())
		{
			::context->getGlobals()->merge(globals);
		}

		::context->emitFileHeader();
		::function->emitIAS();
		auto assembler = ::function->releaseAssembler();
		auto objectWriter = ::context->getObjectWriter();
		assembler->alignFunction();
		objectWriter->writeFunctionCode(::function->getFunctionName(), false, assembler.get());
		::context->lowerGlobals("last");
		::context->lowerConstants();
		objectWriter->setUndefinedSyms(::context->getConstantExternSyms());
		objectWriter->writeNonUserSections();

		return ::routine;
	}

	void Nucleus::optimize()
	{
	}

	Value *Nucleus::allocateStackVariable(Type *t, int arraySize)
	{
		Ice::Type type = T(t);
		int typeSize = Ice::typeWidthInBytes(type);
		int totalSize = typeSize * (arraySize ? arraySize : 1);

		auto bytes = Ice::ConstantInteger32::create(::context, type, totalSize);
		auto address = ::function->makeVariable(T(getPointerType(t)));
		auto alloca = Ice::InstAlloca::create(::function, address, bytes, typeSize);
		::function->getEntryNode()->getInsts().push_front(alloca);

		return V(address);
	}

	BasicBlock *Nucleus::createBasicBlock()
	{
		return B(::function->makeNode());
	}

	BasicBlock *Nucleus::getInsertBlock()
	{
		return B(::basicBlock);
	}

	void Nucleus::setInsertBlock(BasicBlock *basicBlock)
	{
	//	assert(::basicBlock->getInsts().back().getTerminatorEdges().size() >= 0 && "Previous basic block must have a terminator");
		::basicBlock = basicBlock;
	}

	void Nucleus::createFunction(Type *ReturnType, std::vector<Type*> &Params)
	{
		uint32_t sequenceNumber = 0;
		::function = Ice::Cfg::create(::context, sequenceNumber).release();
		::allocator = new Ice::CfgLocalAllocatorScope(::function);

		for(Type *type : Params)
		{
			Ice::Variable *arg = ::function->makeVariable(T(type));
			::function->addArg(arg);
		}

		Ice::CfgNode *node = ::function->makeNode();
		::function->setEntryNode(node);
		::basicBlock = node;
	}

	Value *Nucleus::getArgument(unsigned int index)
	{
		return V(::function->getArgs()[index]);
	}

	void Nucleus::createRetVoid()
	{
		Ice::InstRet *ret = Ice::InstRet::create(::function);
		::basicBlock->appendInst(ret);
	}

	void Nucleus::createRet(Value *v)
	{
		Ice::InstRet *ret = Ice::InstRet::create(::function, v);
		::basicBlock->appendInst(ret);
	}

	void Nucleus::createBr(BasicBlock *dest)
	{
		auto br = Ice::InstBr::create(::function, dest);
		::basicBlock->appendInst(br);
	}

	void Nucleus::createCondBr(Value *cond, BasicBlock *ifTrue, BasicBlock *ifFalse)
	{
		auto br = Ice::InstBr::create(::function, cond, ifTrue, ifFalse);
		::basicBlock->appendInst(br);
	}

	static Value *createArithmetic(Ice::InstArithmetic::OpKind op, Value *lhs, Value *rhs)
	{
		assert(lhs->getType() == rhs->getType() || (llvm::isa<Ice::Constant>(rhs) && (op == Ice::InstArithmetic::Shl || Ice::InstArithmetic::Lshr || Ice::InstArithmetic::Ashr)));

		Ice::Variable *result = ::function->makeVariable(lhs->getType());
		Ice::InstArithmetic *arithmetic = Ice::InstArithmetic::create(::function, op, result, lhs, rhs);
		::basicBlock->appendInst(arithmetic);

		return V(result);
	}

	Value *Nucleus::createAdd(Value *lhs, Value *rhs)
	{
		return createArithmetic(Ice::InstArithmetic::Add, lhs, rhs);
	}

	Value *Nucleus::createSub(Value *lhs, Value *rhs)
	{
		return createArithmetic(Ice::InstArithmetic::Sub, lhs, rhs);
	}

	Value *Nucleus::createMul(Value *lhs, Value *rhs)
	{
		return createArithmetic(Ice::InstArithmetic::Mul, lhs, rhs);
	}

	Value *Nucleus::createUDiv(Value *lhs, Value *rhs)
	{
		return createArithmetic(Ice::InstArithmetic::Udiv, lhs, rhs);
	}

	Value *Nucleus::createSDiv(Value *lhs, Value *rhs)
	{
		return createArithmetic(Ice::InstArithmetic::Sdiv, lhs, rhs);
	}

	Value *Nucleus::createFAdd(Value *lhs, Value *rhs)
	{
		return createArithmetic(Ice::InstArithmetic::Fadd, lhs, rhs);
	}

	Value *Nucleus::createFSub(Value *lhs, Value *rhs)
	{
		return createArithmetic(Ice::InstArithmetic::Fsub, lhs, rhs);
	}

	Value *Nucleus::createFMul(Value *lhs, Value *rhs)
	{
		return createArithmetic(Ice::InstArithmetic::Fmul, lhs, rhs);
	}

	Value *Nucleus::createFDiv(Value *lhs, Value *rhs)
	{
		return createArithmetic(Ice::InstArithmetic::Fdiv, lhs, rhs);
	}

	Value *Nucleus::createURem(Value *lhs, Value *rhs)
	{
		return createArithmetic(Ice::InstArithmetic::Urem, lhs, rhs);
	}

	Value *Nucleus::createSRem(Value *lhs, Value *rhs)
	{
		return createArithmetic(Ice::InstArithmetic::Srem, lhs, rhs);
	}

	Value *Nucleus::createFRem(Value *lhs, Value *rhs)
	{
		return createArithmetic(Ice::InstArithmetic::Frem, lhs, rhs);
	}

	Value *Nucleus::createShl(Value *lhs, Value *rhs)
	{
		return createArithmetic(Ice::InstArithmetic::Shl, lhs, rhs);
	}

	Value *Nucleus::createLShr(Value *lhs, Value *rhs)
	{
		return createArithmetic(Ice::InstArithmetic::Lshr, lhs, rhs);
	}

	Value *Nucleus::createAShr(Value *lhs, Value *rhs)
	{
		return createArithmetic(Ice::InstArithmetic::Ashr, lhs, rhs);
	}

	Value *Nucleus::createAnd(Value *lhs, Value *rhs)
	{
		return createArithmetic(Ice::InstArithmetic::And, lhs, rhs);
	}

	Value *Nucleus::createOr(Value *lhs, Value *rhs)
	{
		return createArithmetic(Ice::InstArithmetic::Or, lhs, rhs);
	}

	Value *Nucleus::createXor(Value *lhs, Value *rhs)
	{
		return createArithmetic(Ice::InstArithmetic::Xor, lhs, rhs);
	}

	static Value *createAssign(Ice::Constant *constant)
	{
		Ice::Variable *value = ::function->makeVariable(constant->getType());
		auto assign = Ice::InstAssign::create(::function, value, constant);
		::basicBlock->appendInst(assign);

		return V(value);
	}

	Value *Nucleus::createNeg(Value *v)
	{
		assert(false && "UNIMPLEMENTED"); return nullptr;
	}

	Value *Nucleus::createFNeg(Value *v)
	{
		assert(false && "UNIMPLEMENTED"); return nullptr;
	}

	Value *Nucleus::createNot(Value *v)
	{
		assert(false && "UNIMPLEMENTED"); return nullptr;
	}

	Value *Nucleus::createLoad(Value *ptr, Type *type, bool isVolatile, unsigned int align)
	{
		int valueType = (int)reinterpret_cast<intptr_t>(type);
		Ice::Variable *result = ::function->makeVariable(T(type));

		if(valueType & EmulatedBits)
		{
			switch(valueType)
			{
			case Type_v4i8:
			case Type_v2i16:
				{
					const Ice::Intrinsics::IntrinsicInfo intrinsic = {Ice::Intrinsics::LoadSubVector, Ice::Intrinsics::SideEffects_F, Ice::Intrinsics::ReturnsTwice_F, Ice::Intrinsics::MemoryWrite_F};
					auto target = ::context->getConstantUndef(Ice::IceType_i32);
					auto load = Ice::InstIntrinsicCall::create(::function, 2, result, target, intrinsic);
					load->addArg(::context->getConstantInt32(4));
					load->addArg(ptr);
					::basicBlock->appendInst(load);
				}
				break;
			case Type_v2i32:
			case Type_v8i8:
			case Type_v4i16:
			case Type_v2f32:
				{
					const Ice::Intrinsics::IntrinsicInfo intrinsic = {Ice::Intrinsics::LoadSubVector, Ice::Intrinsics::SideEffects_F, Ice::Intrinsics::ReturnsTwice_F, Ice::Intrinsics::MemoryWrite_F};
					auto target = ::context->getConstantUndef(Ice::IceType_i32);
					auto load = Ice::InstIntrinsicCall::create(::function, 2, result, target, intrinsic);
					load->addArg(::context->getConstantInt32(8));
					load->addArg(ptr);
					::basicBlock->appendInst(load);
				}
				break;
			default: assert(false && "UNIMPLEMENTED");
			}
		}
		else
		{
			auto load = Ice::InstLoad::create(::function, result, ptr, align);
			::basicBlock->appendInst(load);
		}

		return V(result);
	}

	Value *Nucleus::createStore(Value *value, Value *ptr, Type *type, bool isVolatile, unsigned int align)
	{
		int valueType = (int)reinterpret_cast<intptr_t>(type);

		if(valueType & EmulatedBits)
		{
			switch(valueType)
			{
			case Type_v4i8:
			case Type_v2i16:
				{
					const Ice::Intrinsics::IntrinsicInfo intrinsic = {Ice::Intrinsics::StoreSubVector, Ice::Intrinsics::SideEffects_T, Ice::Intrinsics::ReturnsTwice_F, Ice::Intrinsics::MemoryWrite_T};
					auto target = ::context->getConstantUndef(Ice::IceType_i32);
					auto store = Ice::InstIntrinsicCall::create(::function, 3, nullptr, target, intrinsic);
					store->addArg(::context->getConstantInt32(4));
					store->addArg(value);
					store->addArg(ptr);
					::basicBlock->appendInst(store);
				}
				break;
			case Type_v2i32:
			case Type_v8i8:
			case Type_v4i16:
			case Type_v2f32:
				{
					const Ice::Intrinsics::IntrinsicInfo intrinsic = {Ice::Intrinsics::StoreSubVector, Ice::Intrinsics::SideEffects_T, Ice::Intrinsics::ReturnsTwice_F, Ice::Intrinsics::MemoryWrite_T};
					auto target = ::context->getConstantUndef(Ice::IceType_i32);
					auto store = Ice::InstIntrinsicCall::create(::function, 3, nullptr, target, intrinsic);
					store->addArg(::context->getConstantInt32(8));
					store->addArg(value);
					store->addArg(ptr);
					::basicBlock->appendInst(store);
				}
				break;
			default: assert(false && "UNIMPLEMENTED");
			}
		}
		else
		{
			assert(T(value->getType()) == type);

			auto store = Ice::InstStore::create(::function, value, ptr, align);
			::basicBlock->appendInst(store);
		}

		return value;
	}

	Value *Nucleus::createGEP(Value *ptr, Type *type, Value *index)
	{
		assert(index->getType() == Ice::IceType_i32);

		if(!Ice::isByteSizedType(T(type)))
		{
			index = createMul(index, createConstantInt((int)Ice::typeWidthInBytes(T(type))));
		}

		if(sizeof(void*) == 8)
		{
			index = createSExt(index, T(Ice::IceType_i64));
		}

		return createAdd(ptr, index);
	}

	Value *Nucleus::createAtomicAdd(Value *ptr, Value *value)
	{
		assert(false && "UNIMPLEMENTED"); return nullptr;
	}

	static Value *createCast(Ice::InstCast::OpKind op, Value *v, Type *destType)
	{
		if(v->getType() == T(destType))
		{
			return v;
		}

		Ice::Variable *result = ::function->makeVariable(T(destType));
		Ice::InstCast *cast = Ice::InstCast::create(::function, op, result, v);
		::basicBlock->appendInst(cast);

		return V(result);
	}

	Value *Nucleus::createTrunc(Value *v, Type *destType)
	{
		return createCast(Ice::InstCast::Trunc, v, destType);
	}

	Value *Nucleus::createZExt(Value *v, Type *destType)
	{
		return createCast(Ice::InstCast::Zext, v, destType);
	}

	Value *Nucleus::createSExt(Value *v, Type *destType)
	{
		return createCast(Ice::InstCast::Sext, v, destType);
	}

	Value *Nucleus::createFPToSI(Value *v, Type *destType)
	{
		return createCast(Ice::InstCast::Fptosi, v, destType);
	}

	Value *Nucleus::createUIToFP(Value *v, Type *destType)
	{
		return createCast(Ice::InstCast::Uitofp, v, destType);
	}

	Value *Nucleus::createSIToFP(Value *v, Type *destType)
	{
		return createCast(Ice::InstCast::Sitofp, v, destType);
	}

	Value *Nucleus::createFPTrunc(Value *v, Type *destType)
	{
		return createCast(Ice::InstCast::Fptrunc, v, destType);
	}

	Value *Nucleus::createFPExt(Value *v, Type *destType)
	{
		return createCast(Ice::InstCast::Fpext, v, destType);
	}

	Value *Nucleus::createBitCast(Value *v, Type *destType)
	{
		return createCast(Ice::InstCast::Bitcast, v, destType);
	}

	static Value *createIntCompare(Ice::InstIcmp::ICond condition, Value *lhs, Value *rhs)
	{
		assert(lhs->getType() == rhs->getType());

		auto result = ::function->makeVariable(Ice::isScalarIntegerType(lhs->getType()) ? Ice::IceType_i1 : lhs->getType());
		auto cmp = Ice::InstIcmp::create(::function, condition, result, lhs, rhs);
		::basicBlock->appendInst(cmp);

		return V(result);
	}

	Value *Nucleus::createICmpEQ(Value *lhs, Value *rhs)
	{
		return createIntCompare(Ice::InstIcmp::Eq, lhs, rhs);
	}

	Value *Nucleus::createICmpNE(Value *lhs, Value *rhs)
	{
		return createIntCompare(Ice::InstIcmp::Ne, lhs, rhs);
	}

	Value *Nucleus::createICmpUGT(Value *lhs, Value *rhs)
	{
		return createIntCompare(Ice::InstIcmp::Ugt, lhs, rhs);
	}

	Value *Nucleus::createICmpUGE(Value *lhs, Value *rhs)
	{
		return createIntCompare(Ice::InstIcmp::Uge, lhs, rhs);
	}

	Value *Nucleus::createICmpULT(Value *lhs, Value *rhs)
	{
		return createIntCompare(Ice::InstIcmp::Ult, lhs, rhs);
	}

	Value *Nucleus::createICmpULE(Value *lhs, Value *rhs)
	{
		return createIntCompare(Ice::InstIcmp::Ule, lhs, rhs);
	}

	Value *Nucleus::createICmpSGT(Value *lhs, Value *rhs)
	{
		return createIntCompare(Ice::InstIcmp::Sgt, lhs, rhs);
	}

	Value *Nucleus::createICmpSGE(Value *lhs, Value *rhs)
	{
		return createIntCompare(Ice::InstIcmp::Sge, lhs, rhs);
	}

	Value *Nucleus::createICmpSLT(Value *lhs, Value *rhs)
	{
		return createIntCompare(Ice::InstIcmp::Slt, lhs, rhs);
	}

	Value *Nucleus::createICmpSLE(Value *lhs, Value *rhs)
	{
		return createIntCompare(Ice::InstIcmp::Sle, lhs, rhs);
	}

	static Value *createFloatCompare(Ice::InstFcmp::FCond condition, Value *lhs, Value *rhs)
	{
		assert(lhs->getType() == rhs->getType());
		assert(Ice::isScalarFloatingType(lhs->getType()) || lhs->getType() == Ice::IceType_v4f32);

		auto result = ::function->makeVariable(Ice::isScalarFloatingType(lhs->getType()) ? Ice::IceType_i1 : Ice::IceType_v4i32);
		auto cmp = Ice::InstFcmp::create(::function, condition, result, lhs, rhs);
		::basicBlock->appendInst(cmp);

		return V(result);
	}

	Value *Nucleus::createFCmpOEQ(Value *lhs, Value *rhs)
	{
		return createFloatCompare(Ice::InstFcmp::Oeq, lhs, rhs);
	}

	Value *Nucleus::createFCmpOGT(Value *lhs, Value *rhs)
	{
		return createFloatCompare(Ice::InstFcmp::Ogt, lhs, rhs);
	}

	Value *Nucleus::createFCmpOGE(Value *lhs, Value *rhs)
	{
		return createFloatCompare(Ice::InstFcmp::Oge, lhs, rhs);
	}

	Value *Nucleus::createFCmpOLT(Value *lhs, Value *rhs)
	{
		return createFloatCompare(Ice::InstFcmp::Olt, lhs, rhs);
	}

	Value *Nucleus::createFCmpOLE(Value *lhs, Value *rhs)
	{
		return createFloatCompare(Ice::InstFcmp::Ole, lhs, rhs);
	}

	Value *Nucleus::createFCmpONE(Value *lhs, Value *rhs)
	{
		return createFloatCompare(Ice::InstFcmp::One, lhs, rhs);
	}

	Value *Nucleus::createFCmpORD(Value *lhs, Value *rhs)
	{
		return createFloatCompare(Ice::InstFcmp::Ord, lhs, rhs);
	}

	Value *Nucleus::createFCmpUNO(Value *lhs, Value *rhs)
	{
		return createFloatCompare(Ice::InstFcmp::Uno, lhs, rhs);
	}

	Value *Nucleus::createFCmpUEQ(Value *lhs, Value *rhs)
	{
		return createFloatCompare(Ice::InstFcmp::Ueq, lhs, rhs);
	}

	Value *Nucleus::createFCmpUGT(Value *lhs, Value *rhs)
	{
		return createFloatCompare(Ice::InstFcmp::Ugt, lhs, rhs);
	}

	Value *Nucleus::createFCmpUGE(Value *lhs, Value *rhs)
	{
		return createFloatCompare(Ice::InstFcmp::Uge, lhs, rhs);
	}

	Value *Nucleus::createFCmpULT(Value *lhs, Value *rhs)
	{
		return createFloatCompare(Ice::InstFcmp::Ult, lhs, rhs);
	}

	Value *Nucleus::createFCmpULE(Value *lhs, Value *rhs)
	{
		return createFloatCompare(Ice::InstFcmp::Ule, lhs, rhs);
	}

	Value *Nucleus::createFCmpUNE(Value *lhs, Value *rhs)
	{
		return createFloatCompare(Ice::InstFcmp::Une, lhs, rhs);
	}

	Value *Nucleus::createExtractElement(Value *vector, Type *type, int index)
	{
		auto result = ::function->makeVariable(T(type));
		auto extract = Ice::InstExtractElement::create(::function, result, vector, ::context->getConstantInt32(index));
		::basicBlock->appendInst(extract);

		return V(result);
	}

	Value *Nucleus::createInsertElement(Value *vector, Value *element, int index)
	{
		auto result = ::function->makeVariable(vector->getType());
		auto insert = Ice::InstInsertElement::create(::function, result, vector, element, ::context->getConstantInt32(index));
		::basicBlock->appendInst(insert);

		return V(result);
	}

	Value *Nucleus::createShuffleVector(Value *V1, Value *V2, const int *select)
	{
		assert(V1->getType() == V2->getType());

		int size = Ice::typeNumElements(V1->getType());
		auto result = ::function->makeVariable(V1->getType());
		auto shuffle = Ice::InstShuffleVector::create(::function, result, V1, V2);

		for(int i = 0; i < size; i++)
		{
			shuffle->addIndex(llvm::cast<Ice::ConstantInteger32>(::context->getConstantInt32(select[i])));
		}

		::basicBlock->appendInst(shuffle);

		return V(result);
	}

	Value *Nucleus::createSelect(Value *C, Value *ifTrue, Value *ifFalse)
	{
		assert(ifTrue->getType() == ifFalse->getType());

		auto result = ::function->makeVariable(ifTrue->getType());
		auto *select = Ice::InstSelect::create(::function, result, C, ifTrue, ifFalse);
		::basicBlock->appendInst(select);

		return V(result);
	}

	Value *Nucleus::createSwitch(Value *v, BasicBlock *Dest, unsigned NumCases)
	{
		assert(false && "UNIMPLEMENTED"); return nullptr;
	}

	void Nucleus::addSwitchCase(Value *Switch, int Case, BasicBlock *Branch)
	{
		assert(false && "UNIMPLEMENTED"); return;
	}

	void Nucleus::createUnreachable()
	{
		Ice::InstUnreachable *unreachable = Ice::InstUnreachable::create(::function);
		::basicBlock->appendInst(unreachable);
	}

	static Value *createSwizzle4(Value *val, unsigned char select)
	{
		int swizzle[4] =
		{
			(select >> 0) & 0x03,
			(select >> 2) & 0x03,
			(select >> 4) & 0x03,
			(select >> 6) & 0x03,
		};

		return Nucleus::createShuffleVector(val, val, swizzle);
	}

	static Value *createMask4(Value *lhs, Value *rhs, unsigned char select)
	{
		assert(false && "UNIMPLEMENTED"); return nullptr;
	}

	Value *Nucleus::createConstantPointer(const void *address, Type *Ty, unsigned int align)
	{
		if(sizeof(void*) == 8)
		{
			return createAssign(::context->getConstantInt64(reinterpret_cast<intptr_t>(address)));
		}
		else
		{
			return createAssign(::context->getConstantInt32(reinterpret_cast<intptr_t>(address)));
		}
	}

	Type *Nucleus::getPointerType(Type *ElementType)
	{
		if(sizeof(void*) == 8)
		{
			return T(Ice::IceType_i64);
		}
		else
		{
			return T(Ice::IceType_i32);
		}
	}

	Value *Nucleus::createNullValue(Type *Ty)
	{
		if(Ice::isVectorType(T(Ty)))
		{
			int64_t c[4] = {0, 0, 0, 0};
			return createConstantVector(c, Ty);
		}
		else
		{
			return createAssign(::context->getConstantZero(T(Ty)));
		}
	}

	Value *Nucleus::createConstantLong(int64_t i)
	{
		return createAssign(::context->getConstantInt64(i));
	}

	Value *Nucleus::createConstantInt(int i)
	{
		return createAssign(::context->getConstantInt32(i));
	}

	Value *Nucleus::createConstantInt(unsigned int i)
	{
		return createAssign(::context->getConstantInt32(i));
	}

	Value *Nucleus::createConstantBool(bool b)
	{
		return createAssign(::context->getConstantInt1(b));
	}

	Value *Nucleus::createConstantByte(signed char i)
	{
		return createAssign(::context->getConstantInt8(i));
	}

	Value *Nucleus::createConstantByte(unsigned char i)
	{
		return createAssign(::context->getConstantInt8(i));
	}

	Value *Nucleus::createConstantShort(short i)
	{
		return createAssign(::context->getConstantInt16(i));
	}

	Value *Nucleus::createConstantShort(unsigned short i)
	{
		return createAssign(::context->getConstantInt16(i));
	}

	Value *Nucleus::createConstantFloat(float x)
	{
		return createAssign(::context->getConstantFloat(x));
	}

	Value *Nucleus::createNullPointer(Type *Ty)
	{
		if(true)
		{
			return createNullValue(T(sizeof(void*) == 8 ? Ice::IceType_i64 : Ice::IceType_i32));
		}
		else
		{
			return createConstantPointer(nullptr, Ty);
		}
	}

	Value *Nucleus::createConstantVector(const int64_t *constants, Type *type)
	{
		const int vectorSize = 16;
		assert(Ice::typeWidthInBytes(T(type)) == vectorSize);
		const int alignment = vectorSize;
		auto globalPool = ::function->getGlobalPool();

		const int64_t *i = constants;
		const double *f = reinterpret_cast<const double*>(constants);
		Ice::VariableDeclaration::DataInitializer *dataInitializer = nullptr;

		switch((int)reinterpret_cast<intptr_t>(type))
		{
		case Ice::IceType_v4i32:
			{
				const int initializer[4] = {(int)i[0], (int)i[1], (int)i[2], (int)i[3]};
				static_assert(sizeof(initializer) == vectorSize, "!");
				dataInitializer = Ice::VariableDeclaration::DataInitializer::create(globalPool, (const char*)initializer, vectorSize);
			}
			break;
		case Ice::IceType_v4f32:
			{
				const float initializer[4] = {(float)f[0], (float)f[1], (float)f[2], (float)f[3]};
				static_assert(sizeof(initializer) == vectorSize, "!");
				dataInitializer = Ice::VariableDeclaration::DataInitializer::create(globalPool, (const char*)initializer, vectorSize);
			}
			break;
		case Ice::IceType_v8i16:
			{
				const short initializer[8] = {(short)i[0], (short)i[1], (short)i[2], (short)i[3], (short)i[4], (short)i[5], (short)i[6], (short)i[7]};
				static_assert(sizeof(initializer) == vectorSize, "!");
				dataInitializer = Ice::VariableDeclaration::DataInitializer::create(globalPool, (const char*)initializer, vectorSize);
			}
			break;
		case Ice::IceType_v16i8:
			{
				const char initializer[16] = {(char)i[0], (char)i[1], (char)i[2], (char)i[3], (char)i[4], (char)i[5], (char)i[6], (char)i[7], (char)i[8], (char)i[9], (char)i[10], (char)i[11], (char)i[12], (char)i[13], (char)i[14], (char)i[15]};
				static_assert(sizeof(initializer) == vectorSize, "!");
				dataInitializer = Ice::VariableDeclaration::DataInitializer::create(globalPool, (const char*)initializer, vectorSize);
			}
			break;
		case Type_v2i32:
			{
				const int initializer[4] = {(int)i[0], (int)i[1], (int)i[0], (int)i[1]};
				static_assert(sizeof(initializer) == vectorSize, "!");
				dataInitializer = Ice::VariableDeclaration::DataInitializer::create(globalPool, (const char*)initializer, vectorSize);
			}
			break;
		case Type_v2f32:
			{
				const float initializer[4] = {(float)f[0], (float)f[1], (float)f[0], (float)f[1]};
				static_assert(sizeof(initializer) == vectorSize, "!");
				dataInitializer = Ice::VariableDeclaration::DataInitializer::create(globalPool, (const char*)initializer, vectorSize);
			}
			break;
		case Type_v4i16:
			{
				const short initializer[8] = {(short)i[0], (short)i[1], (short)i[2], (short)i[3], (short)i[0], (short)i[1], (short)i[2], (short)i[3]};
				static_assert(sizeof(initializer) == vectorSize, "!");
				dataInitializer = Ice::VariableDeclaration::DataInitializer::create(globalPool, (const char*)initializer, vectorSize);
			}
			break;
		case Type_v8i8:
			{
				const char initializer[16] = {(char)i[0], (char)i[1], (char)i[2], (char)i[3], (char)i[4], (char)i[5], (char)i[6], (char)i[7], (char)i[0], (char)i[1], (char)i[2], (char)i[3], (char)i[4], (char)i[5], (char)i[6], (char)i[7]};
				static_assert(sizeof(initializer) == vectorSize, "!");
				dataInitializer = Ice::VariableDeclaration::DataInitializer::create(globalPool, (const char*)initializer, vectorSize);
			}
			break;
		case Type_v4i8:
			{
				const char initializer[16] = {(char)i[0], (char)i[1], (char)i[2], (char)i[3], (char)i[0], (char)i[1], (char)i[2], (char)i[3], (char)i[0], (char)i[1], (char)i[2], (char)i[3], (char)i[0], (char)i[1], (char)i[2], (char)i[3]};
				static_assert(sizeof(initializer) == vectorSize, "!");
				dataInitializer = Ice::VariableDeclaration::DataInitializer::create(globalPool, (const char*)initializer, vectorSize);
			}
			break;
		default:
			assert(false && "Unknown constant vector type" && type);
		}

		auto name = Ice::GlobalString::createWithoutString(::context);
		auto *variableDeclaration = Ice::VariableDeclaration::create(globalPool);
		variableDeclaration->setName(name);
		variableDeclaration->setAlignment(alignment);
		variableDeclaration->setIsConstant(true);
		variableDeclaration->addInitializer(dataInitializer);
		
		::function->addGlobal(variableDeclaration);

		constexpr int32_t offset = 0;
		Ice::Operand *ptr = ::context->getConstantSym(offset, name);

		Ice::Variable *result = ::function->makeVariable(T(type));
		auto load = Ice::InstLoad::create(::function, result, ptr, alignment);
		::basicBlock->appendInst(load);

		return V(result);
	}

	Value *Nucleus::createConstantVector(const double *constants, Type *type)
	{
		return createConstantVector((const int64_t*)constants, type);
	}

	Type *Void::getType()
	{
		return T(Ice::IceType_void);
	}

	Bool::Bool(Argument<Bool> argument)
	{
		storeValue(argument.value);
	}

	Bool::Bool()
	{
	}

	Bool::Bool(bool x)
	{
		storeValue(Nucleus::createConstantBool(x));
	}

	Bool::Bool(RValue<Bool> rhs)
	{
		storeValue(rhs.value);
	}

	Bool::Bool(const Bool &rhs)
	{
		Value *value = rhs.loadValue();
		storeValue(value);
	}

	Bool::Bool(const Reference<Bool> &rhs)
	{
		Value *value = rhs.loadValue();
		storeValue(value);
	}

	RValue<Bool> Bool::operator=(RValue<Bool> rhs) const
	{
		storeValue(rhs.value);

		return rhs;
	}

	RValue<Bool> Bool::operator=(const Bool &rhs) const
	{
		Value *value = rhs.loadValue();
		storeValue(value);

		return RValue<Bool>(value);
	}

	RValue<Bool> Bool::operator=(const Reference<Bool> &rhs) const
	{
		Value *value = rhs.loadValue();
		storeValue(value);

		return RValue<Bool>(value);
	}

	RValue<Bool> operator!(RValue<Bool> val)
	{
		return RValue<Bool>(Nucleus::createNot(val.value));
	}

	RValue<Bool> operator&&(RValue<Bool> lhs, RValue<Bool> rhs)
	{
		return RValue<Bool>(Nucleus::createAnd(lhs.value, rhs.value));
	}

	RValue<Bool> operator||(RValue<Bool> lhs, RValue<Bool> rhs)
	{
		return RValue<Bool>(Nucleus::createOr(lhs.value, rhs.value));
	}

	Type *Bool::getType()
	{
		return T(Ice::IceType_i1);
	}

	Byte::Byte(Argument<Byte> argument)
	{
		storeValue(argument.value);
	}

	Byte::Byte(RValue<Int> cast)
	{
		Value *integer = Nucleus::createTrunc(cast.value, Byte::getType());

		storeValue(integer);
	}

	Byte::Byte(RValue<UInt> cast)
	{
		Value *integer = Nucleus::createTrunc(cast.value, Byte::getType());

		storeValue(integer);
	}

	Byte::Byte(RValue<UShort> cast)
	{
		Value *integer = Nucleus::createTrunc(cast.value, Byte::getType());

		storeValue(integer);
	}

	Byte::Byte()
	{
	}

	Byte::Byte(int x)
	{
		storeValue(Nucleus::createConstantByte((unsigned char)x));
	}

	Byte::Byte(unsigned char x)
	{
		storeValue(Nucleus::createConstantByte(x));
	}

	Byte::Byte(RValue<Byte> rhs)
	{
		storeValue(rhs.value);
	}

	Byte::Byte(const Byte &rhs)
	{
		Value *value = rhs.loadValue();
		storeValue(value);
	}

	Byte::Byte(const Reference<Byte> &rhs)
	{
		Value *value = rhs.loadValue();
		storeValue(value);
	}

	RValue<Byte> Byte::operator=(RValue<Byte> rhs) const
	{
		storeValue(rhs.value);

		return rhs;
	}

	RValue<Byte> Byte::operator=(const Byte &rhs) const
	{
		Value *value = rhs.loadValue();
		storeValue(value);

		return RValue<Byte>(value);
	}

	RValue<Byte> Byte::operator=(const Reference<Byte> &rhs) const
	{
		Value *value = rhs.loadValue();
		storeValue(value);

		return RValue<Byte>(value);
	}

	RValue<Byte> operator+(RValue<Byte> lhs, RValue<Byte> rhs)
	{
		return RValue<Byte>(Nucleus::createAdd(lhs.value, rhs.value));
	}

	RValue<Byte> operator-(RValue<Byte> lhs, RValue<Byte> rhs)
	{
		return RValue<Byte>(Nucleus::createSub(lhs.value, rhs.value));
	}

	RValue<Byte> operator*(RValue<Byte> lhs, RValue<Byte> rhs)
	{
		return RValue<Byte>(Nucleus::createMul(lhs.value, rhs.value));
	}

	RValue<Byte> operator/(RValue<Byte> lhs, RValue<Byte> rhs)
	{
		return RValue<Byte>(Nucleus::createUDiv(lhs.value, rhs.value));
	}

	RValue<Byte> operator%(RValue<Byte> lhs, RValue<Byte> rhs)
	{
		return RValue<Byte>(Nucleus::createURem(lhs.value, rhs.value));
	}

	RValue<Byte> operator&(RValue<Byte> lhs, RValue<Byte> rhs)
	{
		return RValue<Byte>(Nucleus::createAnd(lhs.value, rhs.value));
	}

	RValue<Byte> operator|(RValue<Byte> lhs, RValue<Byte> rhs)
	{
		return RValue<Byte>(Nucleus::createOr(lhs.value, rhs.value));
	}

	RValue<Byte> operator^(RValue<Byte> lhs, RValue<Byte> rhs)
	{
		return RValue<Byte>(Nucleus::createXor(lhs.value, rhs.value));
	}

	RValue<Byte> operator<<(RValue<Byte> lhs, RValue<Byte> rhs)
	{
		return RValue<Byte>(Nucleus::createShl(lhs.value, rhs.value));
	}

	RValue<Byte> operator>>(RValue<Byte> lhs, RValue<Byte> rhs)
	{
		return RValue<Byte>(Nucleus::createLShr(lhs.value, rhs.value));
	}

	RValue<Byte> operator+=(const Byte &lhs, RValue<Byte> rhs)
	{
		return lhs = lhs + rhs;
	}

	RValue<Byte> operator-=(const Byte &lhs, RValue<Byte> rhs)
	{
		return lhs = lhs - rhs;
	}

	RValue<Byte> operator*=(const Byte &lhs, RValue<Byte> rhs)
	{
		return lhs = lhs * rhs;
	}

	RValue<Byte> operator/=(const Byte &lhs, RValue<Byte> rhs)
	{
		return lhs = lhs / rhs;
	}

	RValue<Byte> operator%=(const Byte &lhs, RValue<Byte> rhs)
	{
		return lhs = lhs % rhs;
	}

	RValue<Byte> operator&=(const Byte &lhs, RValue<Byte> rhs)
	{
		return lhs = lhs & rhs;
	}

	RValue<Byte> operator|=(const Byte &lhs, RValue<Byte> rhs)
	{
		return lhs = lhs | rhs;
	}

	RValue<Byte> operator^=(const Byte &lhs, RValue<Byte> rhs)
	{
		return lhs = lhs ^ rhs;
	}

	RValue<Byte> operator<<=(const Byte &lhs, RValue<Byte> rhs)
	{
		return lhs = lhs << rhs;
	}

	RValue<Byte> operator>>=(const Byte &lhs, RValue<Byte> rhs)
	{
		return lhs = lhs >> rhs;
	}

	RValue<Byte> operator+(RValue<Byte> val)
	{
		return val;
	}

	RValue<Byte> operator-(RValue<Byte> val)
	{
		return RValue<Byte>(Nucleus::createNeg(val.value));
	}

	RValue<Byte> operator~(RValue<Byte> val)
	{
		return RValue<Byte>(Nucleus::createNot(val.value));
	}

	RValue<Byte> operator++(const Byte &val, int)   // Post-increment
	{
		RValue<Byte> res = val;

		assert(false && "UNIMPLEMENTED");

		return res;
	}

	const Byte &operator++(const Byte &val)   // Pre-increment
	{
		assert(false && "UNIMPLEMENTED");

		return val;
	}

	RValue<Byte> operator--(const Byte &val, int)   // Post-decrement
	{
		RValue<Byte> res = val;

		assert(false && "UNIMPLEMENTED");

		return res;
	}

	const Byte &operator--(const Byte &val)   // Pre-decrement
	{
		assert(false && "UNIMPLEMENTED");

		return val;
	}

	RValue<Bool> operator<(RValue<Byte> lhs, RValue<Byte> rhs)
	{
		return RValue<Bool>(Nucleus::createICmpULT(lhs.value, rhs.value));
	}

	RValue<Bool> operator<=(RValue<Byte> lhs, RValue<Byte> rhs)
	{
		return RValue<Bool>(Nucleus::createICmpULE(lhs.value, rhs.value));
	}

	RValue<Bool> operator>(RValue<Byte> lhs, RValue<Byte> rhs)
	{
		return RValue<Bool>(Nucleus::createICmpUGT(lhs.value, rhs.value));
	}

	RValue<Bool> operator>=(RValue<Byte> lhs, RValue<Byte> rhs)
	{
		return RValue<Bool>(Nucleus::createICmpUGE(lhs.value, rhs.value));
	}

	RValue<Bool> operator!=(RValue<Byte> lhs, RValue<Byte> rhs)
	{
		return RValue<Bool>(Nucleus::createICmpNE(lhs.value, rhs.value));
	}

	RValue<Bool> operator==(RValue<Byte> lhs, RValue<Byte> rhs)
	{
		return RValue<Bool>(Nucleus::createICmpEQ(lhs.value, rhs.value));
	}

	Type *Byte::getType()
	{
		return T(Ice::IceType_i8);
	}

	SByte::SByte(Argument<SByte> argument)
	{
		storeValue(argument.value);
	}

	SByte::SByte(RValue<Int> cast)
	{
		Value *integer = Nucleus::createTrunc(cast.value, SByte::getType());

		storeValue(integer);
	}

	SByte::SByte(RValue<Short> cast)
	{
		Value *integer = Nucleus::createTrunc(cast.value, SByte::getType());

		storeValue(integer);
	}

	SByte::SByte()
	{
	}

	SByte::SByte(signed char x)
	{
		storeValue(Nucleus::createConstantByte(x));
	}

	SByte::SByte(RValue<SByte> rhs)
	{
		storeValue(rhs.value);
	}

	SByte::SByte(const SByte &rhs)
	{
		Value *value = rhs.loadValue();
		storeValue(value);
	}

	SByte::SByte(const Reference<SByte> &rhs)
	{
		Value *value = rhs.loadValue();
		storeValue(value);
	}

	RValue<SByte> SByte::operator=(RValue<SByte> rhs) const
	{
		storeValue(rhs.value);

		return rhs;
	}

	RValue<SByte> SByte::operator=(const SByte &rhs) const
	{
		Value *value = rhs.loadValue();
		storeValue(value);

		return RValue<SByte>(value);
	}

	RValue<SByte> SByte::operator=(const Reference<SByte> &rhs) const
	{
		Value *value = rhs.loadValue();
		storeValue(value);

		return RValue<SByte>(value);
	}

	RValue<SByte> operator+(RValue<SByte> lhs, RValue<SByte> rhs)
	{
		return RValue<SByte>(Nucleus::createAdd(lhs.value, rhs.value));
	}

	RValue<SByte> operator-(RValue<SByte> lhs, RValue<SByte> rhs)
	{
		return RValue<SByte>(Nucleus::createSub(lhs.value, rhs.value));
	}

	RValue<SByte> operator*(RValue<SByte> lhs, RValue<SByte> rhs)
	{
		return RValue<SByte>(Nucleus::createMul(lhs.value, rhs.value));
	}

	RValue<SByte> operator/(RValue<SByte> lhs, RValue<SByte> rhs)
	{
		return RValue<SByte>(Nucleus::createSDiv(lhs.value, rhs.value));
	}

	RValue<SByte> operator%(RValue<SByte> lhs, RValue<SByte> rhs)
	{
		return RValue<SByte>(Nucleus::createSRem(lhs.value, rhs.value));
	}

	RValue<SByte> operator&(RValue<SByte> lhs, RValue<SByte> rhs)
	{
		return RValue<SByte>(Nucleus::createAnd(lhs.value, rhs.value));
	}

	RValue<SByte> operator|(RValue<SByte> lhs, RValue<SByte> rhs)
	{
		return RValue<SByte>(Nucleus::createOr(lhs.value, rhs.value));
	}

	RValue<SByte> operator^(RValue<SByte> lhs, RValue<SByte> rhs)
	{
		return RValue<SByte>(Nucleus::createXor(lhs.value, rhs.value));
	}

	RValue<SByte> operator<<(RValue<SByte> lhs, RValue<SByte> rhs)
	{
		return RValue<SByte>(Nucleus::createShl(lhs.value, rhs.value));
	}

	RValue<SByte> operator>>(RValue<SByte> lhs, RValue<SByte> rhs)
	{
		return RValue<SByte>(Nucleus::createAShr(lhs.value, rhs.value));
	}

	RValue<SByte> operator+=(const SByte &lhs, RValue<SByte> rhs)
	{
		return lhs = lhs + rhs;
	}

	RValue<SByte> operator-=(const SByte &lhs, RValue<SByte> rhs)
	{
		return lhs = lhs - rhs;
	}

	RValue<SByte> operator*=(const SByte &lhs, RValue<SByte> rhs)
	{
		return lhs = lhs * rhs;
	}

	RValue<SByte> operator/=(const SByte &lhs, RValue<SByte> rhs)
	{
		return lhs = lhs / rhs;
	}

	RValue<SByte> operator%=(const SByte &lhs, RValue<SByte> rhs)
	{
		return lhs = lhs % rhs;
	}

	RValue<SByte> operator&=(const SByte &lhs, RValue<SByte> rhs)
	{
		return lhs = lhs & rhs;
	}

	RValue<SByte> operator|=(const SByte &lhs, RValue<SByte> rhs)
	{
		return lhs = lhs | rhs;
	}

	RValue<SByte> operator^=(const SByte &lhs, RValue<SByte> rhs)
	{
		return lhs = lhs ^ rhs;
	}

	RValue<SByte> operator<<=(const SByte &lhs, RValue<SByte> rhs)
	{
		return lhs = lhs << rhs;
	}

	RValue<SByte> operator>>=(const SByte &lhs, RValue<SByte> rhs)
	{
		return lhs = lhs >> rhs;
	}

	RValue<SByte> operator+(RValue<SByte> val)
	{
		return val;
	}

	RValue<SByte> operator-(RValue<SByte> val)
	{
		return RValue<SByte>(Nucleus::createNeg(val.value));
	}

	RValue<SByte> operator~(RValue<SByte> val)
	{
		return RValue<SByte>(Nucleus::createNot(val.value));
	}

	RValue<SByte> operator++(const SByte &val, int)   // Post-increment
	{
		RValue<SByte> res = val;

		assert(false && "UNIMPLEMENTED");

		return res;
	}

	const SByte &operator++(const SByte &val)   // Pre-increment
	{
		assert(false && "UNIMPLEMENTED");
		assert(false && "UNIMPLEMENTED");

		return val;
	}

	RValue<SByte> operator--(const SByte &val, int)   // Post-decrement
	{
		RValue<SByte> res = val;

		assert(false && "UNIMPLEMENTED");
		assert(false && "UNIMPLEMENTED");

		return res;
	}

	const SByte &operator--(const SByte &val)   // Pre-decrement
	{
		assert(false && "UNIMPLEMENTED");
		assert(false && "UNIMPLEMENTED");

		return val;
	}

	RValue<Bool> operator<(RValue<SByte> lhs, RValue<SByte> rhs)
	{
		return RValue<Bool>(Nucleus::createICmpSLT(lhs.value, rhs.value));
	}

	RValue<Bool> operator<=(RValue<SByte> lhs, RValue<SByte> rhs)
	{
		return RValue<Bool>(Nucleus::createICmpSLE(lhs.value, rhs.value));
	}

	RValue<Bool> operator>(RValue<SByte> lhs, RValue<SByte> rhs)
	{
		return RValue<Bool>(Nucleus::createICmpSGT(lhs.value, rhs.value));
	}

	RValue<Bool> operator>=(RValue<SByte> lhs, RValue<SByte> rhs)
	{
		return RValue<Bool>(Nucleus::createICmpSGE(lhs.value, rhs.value));
	}

	RValue<Bool> operator!=(RValue<SByte> lhs, RValue<SByte> rhs)
	{
		return RValue<Bool>(Nucleus::createICmpNE(lhs.value, rhs.value));
	}

	RValue<Bool> operator==(RValue<SByte> lhs, RValue<SByte> rhs)
	{
		return RValue<Bool>(Nucleus::createICmpEQ(lhs.value, rhs.value));
	}

	Type *SByte::getType()
	{
		return T(Ice::IceType_i8);
	}

	Short::Short(Argument<Short> argument)
	{
		storeValue(argument.value);
	}

	Short::Short(RValue<Int> cast)
	{
		Value *integer = Nucleus::createTrunc(cast.value, Short::getType());

		storeValue(integer);
	}

	Short::Short()
	{
	}

	Short::Short(short x)
	{
		storeValue(Nucleus::createConstantShort(x));
	}

	Short::Short(RValue<Short> rhs)
	{
		storeValue(rhs.value);
	}

	Short::Short(const Short &rhs)
	{
		Value *value = rhs.loadValue();
		storeValue(value);
	}

	Short::Short(const Reference<Short> &rhs)
	{
		Value *value = rhs.loadValue();
		storeValue(value);
	}

	RValue<Short> Short::operator=(RValue<Short> rhs) const
	{
		storeValue(rhs.value);

		return rhs;
	}

	RValue<Short> Short::operator=(const Short &rhs) const
	{
		Value *value = rhs.loadValue();
		storeValue(value);

		return RValue<Short>(value);
	}

	RValue<Short> Short::operator=(const Reference<Short> &rhs) const
	{
		Value *value = rhs.loadValue();
		storeValue(value);

		return RValue<Short>(value);
	}

	RValue<Short> operator+(RValue<Short> lhs, RValue<Short> rhs)
	{
		return RValue<Short>(Nucleus::createAdd(lhs.value, rhs.value));
	}

	RValue<Short> operator-(RValue<Short> lhs, RValue<Short> rhs)
	{
		return RValue<Short>(Nucleus::createSub(lhs.value, rhs.value));
	}

	RValue<Short> operator*(RValue<Short> lhs, RValue<Short> rhs)
	{
		return RValue<Short>(Nucleus::createMul(lhs.value, rhs.value));
	}

	RValue<Short> operator/(RValue<Short> lhs, RValue<Short> rhs)
	{
		return RValue<Short>(Nucleus::createSDiv(lhs.value, rhs.value));
	}

	RValue<Short> operator%(RValue<Short> lhs, RValue<Short> rhs)
	{
		return RValue<Short>(Nucleus::createSRem(lhs.value, rhs.value));
	}

	RValue<Short> operator&(RValue<Short> lhs, RValue<Short> rhs)
	{
		return RValue<Short>(Nucleus::createAnd(lhs.value, rhs.value));
	}

	RValue<Short> operator|(RValue<Short> lhs, RValue<Short> rhs)
	{
		return RValue<Short>(Nucleus::createOr(lhs.value, rhs.value));
	}

	RValue<Short> operator^(RValue<Short> lhs, RValue<Short> rhs)
	{
		return RValue<Short>(Nucleus::createXor(lhs.value, rhs.value));
	}

	RValue<Short> operator<<(RValue<Short> lhs, RValue<Short> rhs)
	{
		return RValue<Short>(Nucleus::createShl(lhs.value, rhs.value));
	}

	RValue<Short> operator>>(RValue<Short> lhs, RValue<Short> rhs)
	{
		return RValue<Short>(Nucleus::createAShr(lhs.value, rhs.value));
	}

	RValue<Short> operator+=(const Short &lhs, RValue<Short> rhs)
	{
		return lhs = lhs + rhs;
	}

	RValue<Short> operator-=(const Short &lhs, RValue<Short> rhs)
	{
		return lhs = lhs - rhs;
	}

	RValue<Short> operator*=(const Short &lhs, RValue<Short> rhs)
	{
		return lhs = lhs * rhs;
	}

	RValue<Short> operator/=(const Short &lhs, RValue<Short> rhs)
	{
		return lhs = lhs / rhs;
	}

	RValue<Short> operator%=(const Short &lhs, RValue<Short> rhs)
	{
		return lhs = lhs % rhs;
	}

	RValue<Short> operator&=(const Short &lhs, RValue<Short> rhs)
	{
		return lhs = lhs & rhs;
	}

	RValue<Short> operator|=(const Short &lhs, RValue<Short> rhs)
	{
		return lhs = lhs | rhs;
	}

	RValue<Short> operator^=(const Short &lhs, RValue<Short> rhs)
	{
		return lhs = lhs ^ rhs;
	}

	RValue<Short> operator<<=(const Short &lhs, RValue<Short> rhs)
	{
		return lhs = lhs << rhs;
	}

	RValue<Short> operator>>=(const Short &lhs, RValue<Short> rhs)
	{
		return lhs = lhs >> rhs;
	}

	RValue<Short> operator+(RValue<Short> val)
	{
		return val;
	}

	RValue<Short> operator-(RValue<Short> val)
	{
		return RValue<Short>(Nucleus::createNeg(val.value));
	}

	RValue<Short> operator~(RValue<Short> val)
	{
		return RValue<Short>(Nucleus::createNot(val.value));
	}

	RValue<Short> operator++(const Short &val, int)   // Post-increment
	{
		RValue<Short> res = val;

		assert(false && "UNIMPLEMENTED");
		assert(false && "UNIMPLEMENTED");

		return res;
	}

	const Short &operator++(const Short &val)   // Pre-increment
	{
		assert(false && "UNIMPLEMENTED");
		assert(false && "UNIMPLEMENTED");

		return val;
	}

	RValue<Short> operator--(const Short &val, int)   // Post-decrement
	{
		RValue<Short> res = val;

		assert(false && "UNIMPLEMENTED");
		assert(false && "UNIMPLEMENTED");

		return res;
	}

	const Short &operator--(const Short &val)   // Pre-decrement
	{
		assert(false && "UNIMPLEMENTED");
		assert(false && "UNIMPLEMENTED");

		return val;
	}

	RValue<Bool> operator<(RValue<Short> lhs, RValue<Short> rhs)
	{
		return RValue<Bool>(Nucleus::createICmpSLT(lhs.value, rhs.value));
	}

	RValue<Bool> operator<=(RValue<Short> lhs, RValue<Short> rhs)
	{
		return RValue<Bool>(Nucleus::createICmpSLE(lhs.value, rhs.value));
	}

	RValue<Bool> operator>(RValue<Short> lhs, RValue<Short> rhs)
	{
		return RValue<Bool>(Nucleus::createICmpSGT(lhs.value, rhs.value));
	}

	RValue<Bool> operator>=(RValue<Short> lhs, RValue<Short> rhs)
	{
		return RValue<Bool>(Nucleus::createICmpSGE(lhs.value, rhs.value));
	}

	RValue<Bool> operator!=(RValue<Short> lhs, RValue<Short> rhs)
	{
		return RValue<Bool>(Nucleus::createICmpNE(lhs.value, rhs.value));
	}

	RValue<Bool> operator==(RValue<Short> lhs, RValue<Short> rhs)
	{
		return RValue<Bool>(Nucleus::createICmpEQ(lhs.value, rhs.value));
	}

	Type *Short::getType()
	{
		return T(Ice::IceType_i16);
	}

	UShort::UShort(Argument<UShort> argument)
	{
		storeValue(argument.value);
	}

	UShort::UShort(RValue<UInt> cast)
	{
		Value *integer = Nucleus::createTrunc(cast.value, UShort::getType());

		storeValue(integer);
	}

	UShort::UShort(RValue<Int> cast)
	{
		Value *integer = Nucleus::createTrunc(cast.value, UShort::getType());

		storeValue(integer);
	}

	UShort::UShort()
	{
	}

	UShort::UShort(unsigned short x)
	{
		storeValue(Nucleus::createConstantShort(x));
	}

	UShort::UShort(RValue<UShort> rhs)
	{
		storeValue(rhs.value);
	}

	UShort::UShort(const UShort &rhs)
	{
		Value *value = rhs.loadValue();
		storeValue(value);
	}

	UShort::UShort(const Reference<UShort> &rhs)
	{
		Value *value = rhs.loadValue();
		storeValue(value);
	}

	RValue<UShort> UShort::operator=(RValue<UShort> rhs) const
	{
		storeValue(rhs.value);

		return rhs;
	}

	RValue<UShort> UShort::operator=(const UShort &rhs) const
	{
		Value *value = rhs.loadValue();
		storeValue(value);

		return RValue<UShort>(value);
	}

	RValue<UShort> UShort::operator=(const Reference<UShort> &rhs) const
	{
		Value *value = rhs.loadValue();
		storeValue(value);

		return RValue<UShort>(value);
	}

	RValue<UShort> operator+(RValue<UShort> lhs, RValue<UShort> rhs)
	{
		return RValue<UShort>(Nucleus::createAdd(lhs.value, rhs.value));
	}

	RValue<UShort> operator-(RValue<UShort> lhs, RValue<UShort> rhs)
	{
		return RValue<UShort>(Nucleus::createSub(lhs.value, rhs.value));
	}

	RValue<UShort> operator*(RValue<UShort> lhs, RValue<UShort> rhs)
	{
		return RValue<UShort>(Nucleus::createMul(lhs.value, rhs.value));
	}

	RValue<UShort> operator/(RValue<UShort> lhs, RValue<UShort> rhs)
	{
		return RValue<UShort>(Nucleus::createUDiv(lhs.value, rhs.value));
	}

	RValue<UShort> operator%(RValue<UShort> lhs, RValue<UShort> rhs)
	{
		return RValue<UShort>(Nucleus::createURem(lhs.value, rhs.value));
	}

	RValue<UShort> operator&(RValue<UShort> lhs, RValue<UShort> rhs)
	{
		return RValue<UShort>(Nucleus::createAnd(lhs.value, rhs.value));
	}

	RValue<UShort> operator|(RValue<UShort> lhs, RValue<UShort> rhs)
	{
		return RValue<UShort>(Nucleus::createOr(lhs.value, rhs.value));
	}

	RValue<UShort> operator^(RValue<UShort> lhs, RValue<UShort> rhs)
	{
		return RValue<UShort>(Nucleus::createXor(lhs.value, rhs.value));
	}

	RValue<UShort> operator<<(RValue<UShort> lhs, RValue<UShort> rhs)
	{
		return RValue<UShort>(Nucleus::createShl(lhs.value, rhs.value));
	}

	RValue<UShort> operator>>(RValue<UShort> lhs, RValue<UShort> rhs)
	{
		return RValue<UShort>(Nucleus::createLShr(lhs.value, rhs.value));
	}

	RValue<UShort> operator+=(const UShort &lhs, RValue<UShort> rhs)
	{
		return lhs = lhs + rhs;
	}

	RValue<UShort> operator-=(const UShort &lhs, RValue<UShort> rhs)
	{
		return lhs = lhs - rhs;
	}

	RValue<UShort> operator*=(const UShort &lhs, RValue<UShort> rhs)
	{
		return lhs = lhs * rhs;
	}

	RValue<UShort> operator/=(const UShort &lhs, RValue<UShort> rhs)
	{
		return lhs = lhs / rhs;
	}

	RValue<UShort> operator%=(const UShort &lhs, RValue<UShort> rhs)
	{
		return lhs = lhs % rhs;
	}

	RValue<UShort> operator&=(const UShort &lhs, RValue<UShort> rhs)
	{
		return lhs = lhs & rhs;
	}

	RValue<UShort> operator|=(const UShort &lhs, RValue<UShort> rhs)
	{
		return lhs = lhs | rhs;
	}

	RValue<UShort> operator^=(const UShort &lhs, RValue<UShort> rhs)
	{
		return lhs = lhs ^ rhs;
	}

	RValue<UShort> operator<<=(const UShort &lhs, RValue<UShort> rhs)
	{
		return lhs = lhs << rhs;
	}

	RValue<UShort> operator>>=(const UShort &lhs, RValue<UShort> rhs)
	{
		return lhs = lhs >> rhs;
	}

	RValue<UShort> operator+(RValue<UShort> val)
	{
		return val;
	}

	RValue<UShort> operator-(RValue<UShort> val)
	{
		return RValue<UShort>(Nucleus::createNeg(val.value));
	}

	RValue<UShort> operator~(RValue<UShort> val)
	{
		return RValue<UShort>(Nucleus::createNot(val.value));
	}

	RValue<UShort> operator++(const UShort &val, int)   // Post-increment
	{
		RValue<UShort> res = val;

		assert(false && "UNIMPLEMENTED");
		assert(false && "UNIMPLEMENTED");

		return res;
	}

	const UShort &operator++(const UShort &val)   // Pre-increment
	{
		assert(false && "UNIMPLEMENTED");
		assert(false && "UNIMPLEMENTED");

		return val;
	}

	RValue<UShort> operator--(const UShort &val, int)   // Post-decrement
	{
		RValue<UShort> res = val;

		assert(false && "UNIMPLEMENTED");
		assert(false && "UNIMPLEMENTED");

		return res;
	}

	const UShort &operator--(const UShort &val)   // Pre-decrement
	{
		assert(false && "UNIMPLEMENTED");
		assert(false && "UNIMPLEMENTED");

		return val;
	}

	RValue<Bool> operator<(RValue<UShort> lhs, RValue<UShort> rhs)
	{
		return RValue<Bool>(Nucleus::createICmpULT(lhs.value, rhs.value));
	}

	RValue<Bool> operator<=(RValue<UShort> lhs, RValue<UShort> rhs)
	{
		return RValue<Bool>(Nucleus::createICmpULE(lhs.value, rhs.value));
	}

	RValue<Bool> operator>(RValue<UShort> lhs, RValue<UShort> rhs)
	{
		return RValue<Bool>(Nucleus::createICmpUGT(lhs.value, rhs.value));
	}

	RValue<Bool> operator>=(RValue<UShort> lhs, RValue<UShort> rhs)
	{
		return RValue<Bool>(Nucleus::createICmpUGE(lhs.value, rhs.value));
	}

	RValue<Bool> operator!=(RValue<UShort> lhs, RValue<UShort> rhs)
	{
		return RValue<Bool>(Nucleus::createICmpNE(lhs.value, rhs.value));
	}

	RValue<Bool> operator==(RValue<UShort> lhs, RValue<UShort> rhs)
	{
		return RValue<Bool>(Nucleus::createICmpEQ(lhs.value, rhs.value));
	}

	Type *UShort::getType()
	{
		return T(Ice::IceType_i16);
	}

	Byte4::Byte4(RValue<Byte8> cast)
	{
	//	xyzw.parent = this;

		storeValue(Nucleus::createBitCast(cast.value, getType()));
	}

	Byte4::Byte4(const Reference<Byte4> &rhs)
	{
	//	xyzw.parent = this;

		assert(false && "UNIMPLEMENTED");
	}

	Type *Byte4::getType()
	{
		return T(Type_v4i8);
	}

	Type *SByte4::getType()
	{
		return T(Type_v4i8);
	}

	Byte8::Byte8()
	{
	}

	Byte8::Byte8(uint8_t x0, uint8_t x1, uint8_t x2, uint8_t x3, uint8_t x4, uint8_t x5, uint8_t x6, uint8_t x7)
	{
		int64_t constantVector[8] = {x0, x1, x2, x3, x4, x5, x6, x7};
		storeValue(Nucleus::createConstantVector(constantVector, getType()));
	}

	Byte8::Byte8(RValue<Byte8> rhs)
	{
		storeValue(rhs.value);
	}

	Byte8::Byte8(const Byte8 &rhs)
	{
		Value *value = rhs.loadValue();
		storeValue(value);
	}

	Byte8::Byte8(const Reference<Byte8> &rhs)
	{
		Value *value = rhs.loadValue();
		storeValue(value);
	}

	RValue<Byte8> Byte8::operator=(RValue<Byte8> rhs) const
	{
		storeValue(rhs.value);

		return rhs;
	}

	RValue<Byte8> Byte8::operator=(const Byte8 &rhs) const
	{
		Value *value = rhs.loadValue();
		storeValue(value);

		return RValue<Byte8>(value);
	}

	RValue<Byte8> Byte8::operator=(const Reference<Byte8> &rhs) const
	{
		Value *value = rhs.loadValue();
		storeValue(value);

		return RValue<Byte8>(value);
	}

	RValue<Byte8> operator+(RValue<Byte8> lhs, RValue<Byte8> rhs)
	{
		return RValue<Byte8>(Nucleus::createAdd(lhs.value, rhs.value));
	}

	RValue<Byte8> operator-(RValue<Byte8> lhs, RValue<Byte8> rhs)
	{
		return RValue<Byte8>(Nucleus::createSub(lhs.value, rhs.value));
	}

//	RValue<Byte8> operator*(RValue<Byte8> lhs, RValue<Byte8> rhs)
//	{
//		return RValue<Byte8>(Nucleus::createMul(lhs.value, rhs.value));
//	}

//	RValue<Byte8> operator/(RValue<Byte8> lhs, RValue<Byte8> rhs)
//	{
//		return RValue<Byte8>(Nucleus::createUDiv(lhs.value, rhs.value));
//	}

//	RValue<Byte8> operator%(RValue<Byte8> lhs, RValue<Byte8> rhs)
//	{
//		return RValue<Byte8>(Nucleus::createURem(lhs.value, rhs.value));
//	}

	RValue<Byte8> operator&(RValue<Byte8> lhs, RValue<Byte8> rhs)
	{
		return RValue<Byte8>(Nucleus::createAnd(lhs.value, rhs.value));
	}

	RValue<Byte8> operator|(RValue<Byte8> lhs, RValue<Byte8> rhs)
	{
		return RValue<Byte8>(Nucleus::createOr(lhs.value, rhs.value));
	}

	RValue<Byte8> operator^(RValue<Byte8> lhs, RValue<Byte8> rhs)
	{
		return RValue<Byte8>(Nucleus::createXor(lhs.value, rhs.value));
	}

//	RValue<Byte8> operator<<(RValue<Byte8> lhs, unsigned char rhs)
//	{
//		return RValue<Byte8>(Nucleus::createShl(lhs.value, C(::context->getConstantInt32(rhs))));
//	}

//	RValue<Byte8> operator>>(RValue<Byte8> lhs, unsigned char rhs)
//	{
//		return RValue<Byte8>(Nucleus::createLShr(lhs.value, C(::context->getConstantInt32(rhs))));
//	}

	RValue<Byte8> operator+=(const Byte8 &lhs, RValue<Byte8> rhs)
	{
		return lhs = lhs + rhs;
	}

	RValue<Byte8> operator-=(const Byte8 &lhs, RValue<Byte8> rhs)
	{
		return lhs = lhs - rhs;
	}

//	RValue<Byte8> operator*=(const Byte8 &lhs, RValue<Byte8> rhs)
//	{
//		return lhs = lhs * rhs;
//	}

//	RValue<Byte8> operator/=(const Byte8 &lhs, RValue<Byte8> rhs)
//	{
//		return lhs = lhs / rhs;
//	}

//	RValue<Byte8> operator%=(const Byte8 &lhs, RValue<Byte8> rhs)
//	{
//		return lhs = lhs % rhs;
//	}

	RValue<Byte8> operator&=(const Byte8 &lhs, RValue<Byte8> rhs)
	{
		return lhs = lhs & rhs;
	}

	RValue<Byte8> operator|=(const Byte8 &lhs, RValue<Byte8> rhs)
	{
		return lhs = lhs | rhs;
	}

	RValue<Byte8> operator^=(const Byte8 &lhs, RValue<Byte8> rhs)
	{
		return lhs = lhs ^ rhs;
	}

//	RValue<Byte8> operator<<=(const Byte8 &lhs, RValue<Byte8> rhs)
//	{
//		return lhs = lhs << rhs;
//	}

//	RValue<Byte8> operator>>=(const Byte8 &lhs, RValue<Byte8> rhs)
//	{
//		return lhs = lhs >> rhs;
//	}

//	RValue<Byte8> operator+(RValue<Byte8> val)
//	{
//		return val;
//	}

//	RValue<Byte8> operator-(RValue<Byte8> val)
//	{
//		return RValue<Byte8>(Nucleus::createNeg(val.value));
//	}

	RValue<Byte8> operator~(RValue<Byte8> val)
	{
		return RValue<Byte8>(Nucleus::createNot(val.value));
	}

	RValue<Byte8> AddSat(RValue<Byte8> x, RValue<Byte8> y)
	{
		assert(false && "UNIMPLEMENTED"); return RValue<Byte8>(V(nullptr));
	}

	RValue<Byte8> SubSat(RValue<Byte8> x, RValue<Byte8> y)
	{
		assert(false && "UNIMPLEMENTED"); return RValue<Byte8>(V(nullptr));
	}

	RValue<Short4> Unpack(RValue<Byte4> x)
	{
		int shuffle[16] = {0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7};   // Real type is v16i8
		return RValue<Short4>(Nucleus::createShuffleVector(x.value, x.value, shuffle));
	}

	RValue<Short4> UnpackLow(RValue<Byte8> x, RValue<Byte8> y)
	{
		int shuffle[16] = {0, 16, 1, 17, 2, 18, 3, 19, 4, 20, 5, 21, 6, 22, 7, 23};   // Real type is v16i8
		return RValue<Short4>(Nucleus::createShuffleVector(x.value, y.value, shuffle));
	}

	RValue<Short4> UnpackHigh(RValue<Byte8> x, RValue<Byte8> y)
	{
		int shuffle[16] = {0, 16, 1, 17, 2, 18, 3, 19, 4, 20, 5, 21, 6, 22, 7, 23};   // Real type is v16i8
		auto lowHigh = RValue<Byte16>(Nucleus::createShuffleVector(x.value, y.value, shuffle));
		return As<Short4>(Swizzle(As<Int4>(lowHigh), 0xEE));
	}

	RValue<Int> SignMask(RValue<Byte8> x)
	{
		assert(false && "UNIMPLEMENTED"); return RValue<Int>(V(nullptr));
	}

//	RValue<Byte8> CmpGT(RValue<Byte8> x, RValue<Byte8> y)
//	{
//		assert(false && "UNIMPLEMENTED"); return RValue<Byte8>(V(nullptr));
//	}

	RValue<Byte8> CmpEQ(RValue<Byte8> x, RValue<Byte8> y)
	{
		assert(false && "UNIMPLEMENTED"); return RValue<Byte8>(V(nullptr));
	}

	Type *Byte8::getType()
	{
		return T(Type_v8i8);
	}

	SByte8::SByte8()
	{
	//	xyzw.parent = this;
	}

	SByte8::SByte8(uint8_t x0, uint8_t x1, uint8_t x2, uint8_t x3, uint8_t x4, uint8_t x5, uint8_t x6, uint8_t x7)
	{
	//	xyzw.parent = this;

		int64_t constantVector[8] = { x0, x1, x2, x3, x4, x5, x6, x7 };
		Value *vector = V(Nucleus::createConstantVector(constantVector, getType()));

		storeValue(Nucleus::createBitCast(vector, getType()));
	}

	SByte8::SByte8(RValue<SByte8> rhs)
	{
	//	xyzw.parent = this;

		storeValue(rhs.value);
	}

	SByte8::SByte8(const SByte8 &rhs)
	{
	//	xyzw.parent = this;

		Value *value = rhs.loadValue();
		storeValue(value);
	}

	SByte8::SByte8(const Reference<SByte8> &rhs)
	{
	//	xyzw.parent = this;

		Value *value = rhs.loadValue();
		storeValue(value);
	}

	RValue<SByte8> SByte8::operator=(RValue<SByte8> rhs) const
	{
		storeValue(rhs.value);

		return rhs;
	}

	RValue<SByte8> SByte8::operator=(const SByte8 &rhs) const
	{
		Value *value = rhs.loadValue();
		storeValue(value);

		return RValue<SByte8>(value);
	}

	RValue<SByte8> SByte8::operator=(const Reference<SByte8> &rhs) const
	{
		Value *value = rhs.loadValue();
		storeValue(value);

		return RValue<SByte8>(value);
	}

	RValue<SByte8> operator+(RValue<SByte8> lhs, RValue<SByte8> rhs)
	{
		return RValue<SByte8>(Nucleus::createAdd(lhs.value, rhs.value));
	}

	RValue<SByte8> operator-(RValue<SByte8> lhs, RValue<SByte8> rhs)
	{
		return RValue<SByte8>(Nucleus::createSub(lhs.value, rhs.value));
	}

//	RValue<SByte8> operator*(RValue<SByte8> lhs, RValue<SByte8> rhs)
//	{
//		return RValue<SByte8>(Nucleus::createMul(lhs.value, rhs.value));
//	}

//	RValue<SByte8> operator/(RValue<SByte8> lhs, RValue<SByte8> rhs)
//	{
//		return RValue<SByte8>(Nucleus::createSDiv(lhs.value, rhs.value));
//	}

//	RValue<SByte8> operator%(RValue<SByte8> lhs, RValue<SByte8> rhs)
//	{
//		return RValue<SByte8>(Nucleus::createSRem(lhs.value, rhs.value));
//	}

	RValue<SByte8> operator&(RValue<SByte8> lhs, RValue<SByte8> rhs)
	{
		return RValue<SByte8>(Nucleus::createAnd(lhs.value, rhs.value));
	}

	RValue<SByte8> operator|(RValue<SByte8> lhs, RValue<SByte8> rhs)
	{
		return RValue<SByte8>(Nucleus::createOr(lhs.value, rhs.value));
	}

	RValue<SByte8> operator^(RValue<SByte8> lhs, RValue<SByte8> rhs)
	{
		return RValue<SByte8>(Nucleus::createXor(lhs.value, rhs.value));
	}

//	RValue<SByte8> operator<<(RValue<SByte8> lhs, unsigned char rhs)
//	{
//		return RValue<SByte8>(Nucleus::createShl(lhs.value, C(::context->getConstantInt32(rhs))));
//	}

//	RValue<SByte8> operator>>(RValue<SByte8> lhs, unsigned char rhs)
//	{
//		return RValue<SByte8>(Nucleus::createAShr(lhs.value, C(::context->getConstantInt32(rhs))));
//	}

	RValue<SByte8> operator+=(const SByte8 &lhs, RValue<SByte8> rhs)
	{
		return lhs = lhs + rhs;
	}

	RValue<SByte8> operator-=(const SByte8 &lhs, RValue<SByte8> rhs)
	{
		return lhs = lhs - rhs;
	}

//	RValue<SByte8> operator*=(const SByte8 &lhs, RValue<SByte8> rhs)
//	{
//		return lhs = lhs * rhs;
//	}

//	RValue<SByte8> operator/=(const SByte8 &lhs, RValue<SByte8> rhs)
//	{
//		return lhs = lhs / rhs;
//	}

//	RValue<SByte8> operator%=(const SByte8 &lhs, RValue<SByte8> rhs)
//	{
//		return lhs = lhs % rhs;
//	}

	RValue<SByte8> operator&=(const SByte8 &lhs, RValue<SByte8> rhs)
	{
		return lhs = lhs & rhs;
	}

	RValue<SByte8> operator|=(const SByte8 &lhs, RValue<SByte8> rhs)
	{
		return lhs = lhs | rhs;
	}

	RValue<SByte8> operator^=(const SByte8 &lhs, RValue<SByte8> rhs)
	{
		return lhs = lhs ^ rhs;
	}

//	RValue<SByte8> operator<<=(const SByte8 &lhs, RValue<SByte8> rhs)
//	{
//		return lhs = lhs << rhs;
//	}

//	RValue<SByte8> operator>>=(const SByte8 &lhs, RValue<SByte8> rhs)
//	{
//		return lhs = lhs >> rhs;
//	}

//	RValue<SByte8> operator+(RValue<SByte8> val)
//	{
//		return val;
//	}

//	RValue<SByte8> operator-(RValue<SByte8> val)
//	{
//		return RValue<SByte8>(Nucleus::createNeg(val.value));
//	}

	RValue<SByte8> operator~(RValue<SByte8> val)
	{
		return RValue<SByte8>(Nucleus::createNot(val.value));
	}

	RValue<SByte8> AddSat(RValue<SByte8> x, RValue<SByte8> y)
	{
		assert(false && "UNIMPLEMENTED"); return RValue<SByte8>(V(nullptr));
	}

	RValue<SByte8> SubSat(RValue<SByte8> x, RValue<SByte8> y)
	{
		assert(false && "UNIMPLEMENTED"); return RValue<SByte8>(V(nullptr));
	}

	RValue<Short4> UnpackLow(RValue<SByte8> x, RValue<SByte8> y)
	{
		int shuffle[16] = {0, 16, 1, 17, 2, 18, 3, 19, 4, 20, 5, 21, 6, 22, 7, 23};   // Real type is v16i8
		return RValue<Short4>(Nucleus::createShuffleVector(x.value, y.value, shuffle));
	}

	RValue<Short4> UnpackHigh(RValue<SByte8> x, RValue<SByte8> y)
	{
		int shuffle[16] = {0, 16, 1, 17, 2, 18, 3, 19, 4, 20, 5, 21, 6, 22, 7, 23};   // Real type is v16i8
		auto lowHigh = RValue<Byte16>(Nucleus::createShuffleVector(x.value, y.value, shuffle));
		return As<Short4>(Swizzle(As<Int4>(lowHigh), 0xEE));
	}

	RValue<Int> SignMask(RValue<SByte8> x)
	{
		Ice::Variable *result = ::function->makeVariable(Ice::IceType_i32);
		const Ice::Intrinsics::IntrinsicInfo intrinsic = {Ice::Intrinsics::SignMask, Ice::Intrinsics::SideEffects_F, Ice::Intrinsics::ReturnsTwice_F, Ice::Intrinsics::MemoryWrite_F};
		auto target = ::context->getConstantUndef(Ice::IceType_i32);
		auto movmsk = Ice::InstIntrinsicCall::create(::function, 1, result, target, intrinsic);
		movmsk->addArg(x.value);
		::basicBlock->appendInst(movmsk);

		return RValue<Int>(V(result));
	}

	RValue<Byte8> CmpGT(RValue<SByte8> x, RValue<SByte8> y)
	{
		assert(false && "UNIMPLEMENTED"); return RValue<Byte8>(V(nullptr));
	}

	RValue<Byte8> CmpEQ(RValue<SByte8> x, RValue<SByte8> y)
	{
		assert(false && "UNIMPLEMENTED"); return RValue<Byte8>(V(nullptr));
	}

	Type *SByte8::getType()
	{
		return T(Type_v8i8);
	}

	Byte16::Byte16(RValue<Byte16> rhs)
	{
	//	xyzw.parent = this;

		storeValue(rhs.value);
	}

	Byte16::Byte16(const Byte16 &rhs)
	{
	//	xyzw.parent = this;

		Value *value = rhs.loadValue();
		storeValue(value);
	}

	Byte16::Byte16(const Reference<Byte16> &rhs)
	{
	//	xyzw.parent = this;

		Value *value = rhs.loadValue();
		storeValue(value);
	}

	RValue<Byte16> Byte16::operator=(RValue<Byte16> rhs) const
	{
		storeValue(rhs.value);

		return rhs;
	}

	RValue<Byte16> Byte16::operator=(const Byte16 &rhs) const
	{
		Value *value = rhs.loadValue();
		storeValue(value);

		return RValue<Byte16>(value);
	}

	RValue<Byte16> Byte16::operator=(const Reference<Byte16> &rhs) const
	{
		Value *value = rhs.loadValue();
		storeValue(value);

		return RValue<Byte16>(value);
	}

	Type *Byte16::getType()
	{
		return T(Ice::IceType_v16i8);
	}

	Type *SByte16::getType()
	{
		return T(Ice::IceType_v16i8);
	}

	Short2::Short2(RValue<Short4> cast)
	{
		assert(false && "UNIMPLEMENTED");
	}

	Type *Short2::getType()
	{
		return T(Type_v2i16);
	}

	UShort2::UShort2(RValue<UShort4> cast)
	{
		assert(false && "UNIMPLEMENTED");
	}

	Type *UShort2::getType()
	{
		return T(Type_v2i16);
	}

	Short4::Short4(RValue<Int> cast)
	{
		Value *extend = Nucleus::createZExt(cast.value, Long::getType());
		Value *swizzle = Swizzle(RValue<Short4>(extend), 0x00).value;

		storeValue(swizzle);
	}

	Short4::Short4(RValue<Int4> cast)
	{
		assert(false && "UNIMPLEMENTED");
	}

//	Short4::Short4(RValue<Float> cast)
//	{
//	}

	Short4::Short4(RValue<Float4> cast)
	{
		assert(false && "UNIMPLEMENTED");
	}

	Short4::Short4()
	{
	//	xyzw.parent = this;
	}

	Short4::Short4(short xyzw)
	{
		//	xyzw.parent = this;

		int64_t constantVector[4] = {xyzw, xyzw, xyzw, xyzw};
		storeValue(Nucleus::createConstantVector(constantVector, getType()));
	}

	Short4::Short4(short x, short y, short z, short w)
	{
		//	xyzw.parent = this;

		int64_t constantVector[4] = {x, y, z, w};
		storeValue(Nucleus::createConstantVector(constantVector, getType()));
	}

	Short4::Short4(RValue<Short4> rhs)
	{
	//	xyzw.parent = this;

		storeValue(rhs.value);
	}

	Short4::Short4(const Short4 &rhs)
	{
	//	xyzw.parent = this;

		Value *value = rhs.loadValue();
		storeValue(value);
	}

	Short4::Short4(const Reference<Short4> &rhs)
	{
	//	xyzw.parent = this;

		Value *value = rhs.loadValue();
		storeValue(value);
	}

	Short4::Short4(RValue<UShort4> rhs)
	{
	//	xyzw.parent = this;

		storeValue(rhs.value);
	}

	Short4::Short4(const UShort4 &rhs)
	{
	//	xyzw.parent = this;

		storeValue(rhs.loadValue());
	}

	Short4::Short4(const Reference<UShort4> &rhs)
	{
	//	xyzw.parent = this;

		storeValue(rhs.loadValue());
	}

	RValue<Short4> Short4::operator=(RValue<Short4> rhs) const
	{
		storeValue(rhs.value);

		return rhs;
	}

	RValue<Short4> Short4::operator=(const Short4 &rhs) const
	{
		Value *value = rhs.loadValue();
		storeValue(value);

		return RValue<Short4>(value);
	}

	RValue<Short4> Short4::operator=(const Reference<Short4> &rhs) const
	{
		Value *value = rhs.loadValue();
		storeValue(value);

		return RValue<Short4>(value);
	}

	RValue<Short4> Short4::operator=(RValue<UShort4> rhs) const
	{
		storeValue(rhs.value);

		return RValue<Short4>(rhs);
	}

	RValue<Short4> Short4::operator=(const UShort4 &rhs) const
	{
		Value *value = rhs.loadValue();
		storeValue(value);

		return RValue<Short4>(value);
	}

	RValue<Short4> Short4::operator=(const Reference<UShort4> &rhs) const
	{
		Value *value = rhs.loadValue();
		storeValue(value);

		return RValue<Short4>(value);
	}

	RValue<Short4> operator+(RValue<Short4> lhs, RValue<Short4> rhs)
	{
		return RValue<Short4>(Nucleus::createAdd(lhs.value, rhs.value));
	}

	RValue<Short4> operator-(RValue<Short4> lhs, RValue<Short4> rhs)
	{
		return RValue<Short4>(Nucleus::createSub(lhs.value, rhs.value));
	}

	RValue<Short4> operator*(RValue<Short4> lhs, RValue<Short4> rhs)
	{
		return RValue<Short4>(Nucleus::createMul(lhs.value, rhs.value));
	}

//	RValue<Short4> operator/(RValue<Short4> lhs, RValue<Short4> rhs)
//	{
//		return RValue<Short4>(Nucleus::createSDiv(lhs.value, rhs.value));
//	}

//	RValue<Short4> operator%(RValue<Short4> lhs, RValue<Short4> rhs)
//	{
//		return RValue<Short4>(Nucleus::createSRem(lhs.value, rhs.value));
//	}

	RValue<Short4> operator&(RValue<Short4> lhs, RValue<Short4> rhs)
	{
		return RValue<Short4>(Nucleus::createAnd(lhs.value, rhs.value));
	}

	RValue<Short4> operator|(RValue<Short4> lhs, RValue<Short4> rhs)
	{
		return RValue<Short4>(Nucleus::createOr(lhs.value, rhs.value));
	}

	RValue<Short4> operator^(RValue<Short4> lhs, RValue<Short4> rhs)
	{
		return RValue<Short4>(Nucleus::createXor(lhs.value, rhs.value));
	}

	RValue<Short4> operator<<(RValue<Short4> lhs, unsigned char rhs)
	{
		return RValue<Short4>(Nucleus::createShl(lhs.value, C(::context->getConstantInt32(rhs))));
	}

	RValue<Short4> operator>>(RValue<Short4> lhs, unsigned char rhs)
	{
		return RValue<Short4>(Nucleus::createAShr(lhs.value, C(::context->getConstantInt32(rhs))));
	}

	RValue<Short4> operator<<(RValue<Short4> lhs, RValue<Long1> rhs)
	{
	//	return RValue<Short4>(Nucleus::createShl(lhs.value, rhs.value));

		assert(false && "UNIMPLEMENTED"); return RValue<Short4>(V(nullptr));
	}

	RValue<Short4> operator>>(RValue<Short4> lhs, RValue<Long1> rhs)
	{
	//	return RValue<Short4>(Nucleus::createAShr(lhs.value, rhs.value));

		assert(false && "UNIMPLEMENTED"); return RValue<Short4>(V(nullptr));
	}

	RValue<Short4> operator+=(const Short4 &lhs, RValue<Short4> rhs)
	{
		return lhs = lhs + rhs;
	}

	RValue<Short4> operator-=(const Short4 &lhs, RValue<Short4> rhs)
	{
		return lhs = lhs - rhs;
	}

	RValue<Short4> operator*=(const Short4 &lhs, RValue<Short4> rhs)
	{
		return lhs = lhs * rhs;
	}

//	RValue<Short4> operator/=(const Short4 &lhs, RValue<Short4> rhs)
//	{
//		return lhs = lhs / rhs;
//	}

//	RValue<Short4> operator%=(const Short4 &lhs, RValue<Short4> rhs)
//	{
//		return lhs = lhs % rhs;
//	}

	RValue<Short4> operator&=(const Short4 &lhs, RValue<Short4> rhs)
	{
		return lhs = lhs & rhs;
	}

	RValue<Short4> operator|=(const Short4 &lhs, RValue<Short4> rhs)
	{
		return lhs = lhs | rhs;
	}

	RValue<Short4> operator^=(const Short4 &lhs, RValue<Short4> rhs)
	{
		return lhs = lhs ^ rhs;
	}

	RValue<Short4> operator<<=(const Short4 &lhs, unsigned char rhs)
	{
		return lhs = lhs << rhs;
	}

	RValue<Short4> operator>>=(const Short4 &lhs, unsigned char rhs)
	{
		return lhs = lhs >> rhs;
	}

	RValue<Short4> operator<<=(const Short4 &lhs, RValue<Long1> rhs)
	{
		return lhs = lhs << rhs;
	}

	RValue<Short4> operator>>=(const Short4 &lhs, RValue<Long1> rhs)
	{
		return lhs = lhs >> rhs;
	}

//	RValue<Short4> operator+(RValue<Short4> val)
//	{
//		return val;
//	}

	RValue<Short4> operator-(RValue<Short4> val)
	{
		assert(false && "UNIMPLEMENTED"); return RValue<Short4>(V(nullptr));
	}

	RValue<Short4> operator~(RValue<Short4> val)
	{
		assert(false && "UNIMPLEMENTED"); return RValue<Short4>(V(nullptr));
	}

	RValue<Short4> RoundShort4(RValue<Float4> cast)
	{
		assert(false && "UNIMPLEMENTED"); return RValue<Short4>(V(nullptr));
	}

	RValue<Short4> Max(RValue<Short4> x, RValue<Short4> y)
	{
		Ice::Variable *condition = ::function->makeVariable(Ice::IceType_v8i1);
		auto cmp = Ice::InstIcmp::create(::function, Ice::InstIcmp::Sle, condition, x.value, y.value);
		::basicBlock->appendInst(cmp);

		Ice::Variable *result = ::function->makeVariable(Ice::IceType_v8i16);
		auto select = Ice::InstSelect::create(::function, result, condition, y.value, x.value);
		::basicBlock->appendInst(select);

		return RValue<Short4>(V(result));
	}

	RValue<Short4> Min(RValue<Short4> x, RValue<Short4> y)
	{
		Ice::Variable *condition = ::function->makeVariable(Ice::IceType_v8i1);
		auto cmp = Ice::InstIcmp::create(::function, Ice::InstIcmp::Sgt, condition, x.value, y.value);
		::basicBlock->appendInst(cmp);

		Ice::Variable *result = ::function->makeVariable(Ice::IceType_v8i16);
		auto select = Ice::InstSelect::create(::function, result, condition, y.value, x.value);
		::basicBlock->appendInst(select);

		return RValue<Short4>(V(result));
	}

	RValue<Short4> AddSat(RValue<Short4> x, RValue<Short4> y)
	{
		assert(false && "UNIMPLEMENTED"); return RValue<Short4>(V(nullptr));
	}

	RValue<Short4> SubSat(RValue<Short4> x, RValue<Short4> y)
	{
		assert(false && "UNIMPLEMENTED"); return RValue<Short4>(V(nullptr));
	}

	RValue<Short4> MulHigh(RValue<Short4> x, RValue<Short4> y)
	{
		assert(false && "UNIMPLEMENTED"); return RValue<Short4>(V(nullptr));
	}

	RValue<Int2> MulAdd(RValue<Short4> x, RValue<Short4> y)
	{
		assert(false && "UNIMPLEMENTED"); return RValue<Int2>(V(nullptr));
	}

	RValue<SByte8> Pack(RValue<Short4> x, RValue<Short4> y)
	{
		Ice::Variable *result = ::function->makeVariable(Ice::IceType_v16i8);
		const Ice::Intrinsics::IntrinsicInfo intrinsic = {Ice::Intrinsics::VectorPackSigned, Ice::Intrinsics::SideEffects_F, Ice::Intrinsics::ReturnsTwice_F, Ice::Intrinsics::MemoryWrite_F};
		auto target = ::context->getConstantUndef(Ice::IceType_i32);
		auto pack = Ice::InstIntrinsicCall::create(::function, 2, result, target, intrinsic);
		pack->addArg(x.value);
		pack->addArg(y.value);
		::basicBlock->appendInst(pack);

		return As<SByte8>(Swizzle(As<Int4>(V(result)), 0x88));
	}

	RValue<Int2> UnpackLow(RValue<Short4> x, RValue<Short4> y)
	{
		int shuffle[8] = {0, 8, 1, 9, 2, 10, 3, 11};   // Real type is v8i16
		return RValue<Int2>(Nucleus::createShuffleVector(x.value, y.value, shuffle));
	}

	RValue<Int2> UnpackHigh(RValue<Short4> x, RValue<Short4> y)
	{
		int shuffle[8] = {0, 8, 1, 9, 2, 10, 3, 11};   // Real type is v8i16
		auto lowHigh = RValue<Short8>(Nucleus::createShuffleVector(x.value, y.value, shuffle));
		return As<Int2>(Swizzle(As<Int4>(lowHigh), 0xEE));
	}

	RValue<Short4> Swizzle(RValue<Short4> x, unsigned char select)
	{
		// Real type is v8i16
		int shuffle[8] =
		{
			(select >> 0) & 0x03,
			(select >> 2) & 0x03,
			(select >> 4) & 0x03,
			(select >> 6) & 0x03,
			(select >> 0) & 0x03,
			(select >> 2) & 0x03,
			(select >> 4) & 0x03,
			(select >> 6) & 0x03,
		};

		return RValue<Short4>(Nucleus::createShuffleVector(x.value, x.value, shuffle));
	}

	RValue<Short4> Insert(RValue<Short4> val, RValue<Short> element, int i)
	{
		assert(false && "UNIMPLEMENTED"); return RValue<Short4>(V(nullptr));
	}

	RValue<Short> Extract(RValue<Short4> val, int i)
	{
		assert(false && "UNIMPLEMENTED"); return RValue<Short>(V(nullptr));
	}

	RValue<Short4> CmpGT(RValue<Short4> x, RValue<Short4> y)
	{
		assert(false && "UNIMPLEMENTED"); return RValue<Short4>(V(nullptr));
	}

	RValue<Short4> CmpEQ(RValue<Short4> x, RValue<Short4> y)
	{
		assert(false && "UNIMPLEMENTED"); return RValue<Short4>(V(nullptr));
	}

	Type *Short4::getType()
	{
		return T(Type_v4i16);
	}

	UShort4::UShort4(RValue<Int4> cast)
	{
		*this = Short4(cast);
	}

	UShort4::UShort4(RValue<Float4> cast, bool saturate)
	{
		assert(false && "UNIMPLEMENTED");
	}

	UShort4::UShort4()
	{
	//	xyzw.parent = this;
	}

	UShort4::UShort4(unsigned short xyzw)
	{
	//	xyzw.parent = this;

		int64_t constantVector[4] = {xyzw, xyzw, xyzw, xyzw};
		storeValue(Nucleus::createConstantVector(constantVector, getType()));
	}

	UShort4::UShort4(unsigned short x, unsigned short y, unsigned short z, unsigned short w)
	{
	//	xyzw.parent = this;

		int64_t constantVector[4] = {x, y, z, w};
		storeValue(Nucleus::createConstantVector(constantVector, getType()));
	}

	UShort4::UShort4(RValue<UShort4> rhs)
	{
	//	xyzw.parent = this;

		storeValue(rhs.value);
	}

	UShort4::UShort4(const UShort4 &rhs)
	{
	//	xyzw.parent = this;

		Value *value = rhs.loadValue();
		storeValue(value);
	}

	UShort4::UShort4(const Reference<UShort4> &rhs)
	{
	//	xyzw.parent = this;

		Value *value = rhs.loadValue();
		storeValue(value);
	}

	UShort4::UShort4(RValue<Short4> rhs)
	{
	//	xyzw.parent = this;

		storeValue(rhs.value);
	}

	UShort4::UShort4(const Short4 &rhs)
	{
	//	xyzw.parent = this;

		Value *value = rhs.loadValue();
		storeValue(value);
	}

	UShort4::UShort4(const Reference<Short4> &rhs)
	{
	//	xyzw.parent = this;

		Value *value = rhs.loadValue();
		storeValue(value);
	}

	RValue<UShort4> UShort4::operator=(RValue<UShort4> rhs) const
	{
		storeValue(rhs.value);

		return rhs;
	}

	RValue<UShort4> UShort4::operator=(const UShort4 &rhs) const
	{
		Value *value = rhs.loadValue();
		storeValue(value);

		return RValue<UShort4>(value);
	}

	RValue<UShort4> UShort4::operator=(const Reference<UShort4> &rhs) const
	{
		Value *value = rhs.loadValue();
		storeValue(value);

		return RValue<UShort4>(value);
	}

	RValue<UShort4> UShort4::operator=(RValue<Short4> rhs) const
	{
		storeValue(rhs.value);

		return RValue<UShort4>(rhs);
	}

	RValue<UShort4> UShort4::operator=(const Short4 &rhs) const
	{
		Value *value = rhs.loadValue();
		storeValue(value);

		return RValue<UShort4>(value);
	}

	RValue<UShort4> UShort4::operator=(const Reference<Short4> &rhs) const
	{
		Value *value = rhs.loadValue();
		storeValue(value);

		return RValue<UShort4>(value);
	}

	RValue<UShort4> operator+(RValue<UShort4> lhs, RValue<UShort4> rhs)
	{
		return RValue<Short4>(Nucleus::createAdd(lhs.value, rhs.value));
	}

	RValue<UShort4> operator-(RValue<UShort4> lhs, RValue<UShort4> rhs)
	{
		return RValue<UShort4>(Nucleus::createSub(lhs.value, rhs.value));
	}

	RValue<UShort4> operator*(RValue<UShort4> lhs, RValue<UShort4> rhs)
	{
		return RValue<UShort4>(Nucleus::createMul(lhs.value, rhs.value));
	}

	RValue<UShort4> operator&(RValue<UShort4> lhs, RValue<UShort4> rhs)
	{
		return RValue<UShort4>(Nucleus::createAnd(lhs.value, rhs.value));
	}

	RValue<UShort4> operator|(RValue<UShort4> lhs, RValue<UShort4> rhs)
	{
		return RValue<UShort4>(Nucleus::createOr(lhs.value, rhs.value));
	}

	RValue<UShort4> operator^(RValue<UShort4> lhs, RValue<UShort4> rhs)
	{
		return RValue<UShort4>(Nucleus::createXor(lhs.value, rhs.value));
	}

	RValue<UShort4> operator<<(RValue<UShort4> lhs, unsigned char rhs)
	{
		return RValue<UShort4>(Nucleus::createShl(lhs.value, C(::context->getConstantInt32(rhs))));
	}

	RValue<UShort4> operator>>(RValue<UShort4> lhs, unsigned char rhs)
	{
		return RValue<UShort4>(Nucleus::createLShr(lhs.value, C(::context->getConstantInt32(rhs))));
	}

	RValue<UShort4> operator<<(RValue<UShort4> lhs, RValue<Long1> rhs)
	{
		assert(false && "UNIMPLEMENTED"); return RValue<UShort4>(V(nullptr));
	}

	RValue<UShort4> operator>>(RValue<UShort4> lhs, RValue<Long1> rhs)
	{
		assert(false && "UNIMPLEMENTED"); return RValue<UShort4>(V(nullptr));
	}

	RValue<UShort4> operator<<=(const UShort4 &lhs, unsigned char rhs)
	{
		return lhs = lhs << rhs;
	}

	RValue<UShort4> operator>>=(const UShort4 &lhs, unsigned char rhs)
	{
		return lhs = lhs >> rhs;
	}

	RValue<UShort4> operator<<=(const UShort4 &lhs, RValue<Long1> rhs)
	{
		return lhs = lhs << rhs;
	}

	RValue<UShort4> operator>>=(const UShort4 &lhs, RValue<Long1> rhs)
	{
		return lhs = lhs >> rhs;
	}

	RValue<UShort4> operator~(RValue<UShort4> val)
	{
		assert(false && "UNIMPLEMENTED"); return RValue<UShort4>(V(nullptr));
	}

	RValue<UShort4> Max(RValue<UShort4> x, RValue<UShort4> y)
	{
		Ice::Variable *condition = ::function->makeVariable(Ice::IceType_v8i1);
		auto cmp = Ice::InstIcmp::create(::function, Ice::InstIcmp::Ule, condition, x.value, y.value);
		::basicBlock->appendInst(cmp);

		Ice::Variable *result = ::function->makeVariable(Ice::IceType_v8i16);
		auto select = Ice::InstSelect::create(::function, result, condition, y.value, x.value);
		::basicBlock->appendInst(select);

		return RValue<UShort4>(V(result));
	}

	RValue<UShort4> Min(RValue<UShort4> x, RValue<UShort4> y)
	{
		Ice::Variable *condition = ::function->makeVariable(Ice::IceType_v8i1);
		auto cmp = Ice::InstIcmp::create(::function, Ice::InstIcmp::Ugt, condition, x.value, y.value);
		::basicBlock->appendInst(cmp);

		Ice::Variable *result = ::function->makeVariable(Ice::IceType_v8i16);
		auto select = Ice::InstSelect::create(::function, result, condition, y.value, x.value);
		::basicBlock->appendInst(select);

		return RValue<UShort4>(V(result));
	}

	RValue<UShort4> AddSat(RValue<UShort4> x, RValue<UShort4> y)
	{
		assert(false && "UNIMPLEMENTED"); return RValue<UShort4>(V(nullptr));
	}

	RValue<UShort4> SubSat(RValue<UShort4> x, RValue<UShort4> y)
	{
		assert(false && "UNIMPLEMENTED"); return RValue<UShort4>(V(nullptr));
	}

	RValue<UShort4> MulHigh(RValue<UShort4> x, RValue<UShort4> y)
	{
		assert(false && "UNIMPLEMENTED"); return RValue<UShort4>(V(nullptr));
	}

	RValue<UShort4> Average(RValue<UShort4> x, RValue<UShort4> y)
	{
		assert(false && "UNIMPLEMENTED"); return RValue<UShort4>(V(nullptr));
	}

	RValue<Byte8> Pack(RValue<UShort4> x, RValue<UShort4> y)
	{
		Ice::Variable *result = ::function->makeVariable(Ice::IceType_v16i8);
		const Ice::Intrinsics::IntrinsicInfo intrinsic = {Ice::Intrinsics::VectorPackUnsigned, Ice::Intrinsics::SideEffects_F, Ice::Intrinsics::ReturnsTwice_F, Ice::Intrinsics::MemoryWrite_F};
		auto target = ::context->getConstantUndef(Ice::IceType_i32);
		auto pack = Ice::InstIntrinsicCall::create(::function, 2, result, target, intrinsic);
		pack->addArg(x.value);
		pack->addArg(y.value);
		::basicBlock->appendInst(pack);

		return As<Byte8>(Swizzle(As<Int4>(V(result)), 0x88));
	}

	Type *UShort4::getType()
	{
		return T(Type_v4i16);
	}

	Short8::Short8(short c0, short c1, short c2, short c3, short c4, short c5, short c6, short c7)
	{
	//	xyzw.parent = this;

		int64_t constantVector[8] = {c0, c1, c2, c3, c4, c5, c6, c7};
		storeValue(Nucleus::createConstantVector(constantVector, getType()));
	}

	Short8::Short8(RValue<Short8> rhs)
	{
	//	xyzw.parent = this;

		storeValue(rhs.value);
	}

	Short8::Short8(const Reference<Short8> &rhs)
	{
	//	xyzw.parent = this;

		Value *value = rhs.loadValue();
		storeValue(value);
	}

	Short8::Short8(RValue<Short4> lo, RValue<Short4> hi)
	{
		assert(false && "UNIMPLEMENTED");
	}

	RValue<Short8> operator+(RValue<Short8> lhs, RValue<Short8> rhs)
	{
		return RValue<Short8>(Nucleus::createAdd(lhs.value, rhs.value));
	}

	RValue<Short8> operator&(RValue<Short8> lhs, RValue<Short8> rhs)
	{
		return RValue<Short8>(Nucleus::createAnd(lhs.value, rhs.value));
	}

	RValue<Short8> operator<<(RValue<Short8> lhs, unsigned char rhs)
	{
		return RValue<Short8>(Nucleus::createShl(lhs.value, C(::context->getConstantInt32(rhs))));
	}

	RValue<Short8> operator>>(RValue<Short8> lhs, unsigned char rhs)
	{
		return RValue<Short8>(Nucleus::createAShr(lhs.value, C(::context->getConstantInt32(rhs))));
	}

	RValue<Int4> MulAdd(RValue<Short8> x, RValue<Short8> y)
	{
		assert(false && "UNIMPLEMENTED"); return RValue<Int4>(V(nullptr));
	}

	RValue<Int4> Abs(RValue<Int4> x)
	{
		assert(false && "UNIMPLEMENTED"); return RValue<Int4>(V(nullptr));
	}

	RValue<Short8> MulHigh(RValue<Short8> x, RValue<Short8> y)
	{
		assert(false && "UNIMPLEMENTED"); return RValue<Short8>(V(nullptr));
	}

	Type *Short8::getType()
	{
		return T(Ice::IceType_v8i16);
	}

	UShort8::UShort8(unsigned short c0, unsigned short c1, unsigned short c2, unsigned short c3, unsigned short c4, unsigned short c5, unsigned short c6, unsigned short c7)
	{
		int64_t constantVector[8] = {c0, c1, c2, c3, c4, c5, c6, c7};
		storeValue(Nucleus::createConstantVector(constantVector, getType()));
	}

	UShort8::UShort8(RValue<UShort8> rhs)
	{
		storeValue(rhs.value);
	}

	UShort8::UShort8(const Reference<UShort8> &rhs)
	{
		Value *value = rhs.loadValue();
		storeValue(value);
	}

	UShort8::UShort8(RValue<UShort4> lo, RValue<UShort4> hi)
	{
		assert(false && "UNIMPLEMENTED");
	}

	RValue<UShort8> UShort8::operator=(RValue<UShort8> rhs) const
	{
		storeValue(rhs.value);

		return rhs;
	}

	RValue<UShort8> UShort8::operator=(const UShort8 &rhs) const
	{
		Value *value = rhs.loadValue();
		storeValue(value);

		return RValue<UShort8>(value);
	}

	RValue<UShort8> UShort8::operator=(const Reference<UShort8> &rhs) const
	{
		Value *value = rhs.loadValue();
		storeValue(value);

		return RValue<UShort8>(value);
	}

	RValue<UShort8> operator&(RValue<UShort8> lhs, RValue<UShort8> rhs)
	{
		return RValue<UShort8>(Nucleus::createAnd(lhs.value, rhs.value));
	}

	RValue<UShort8> operator<<(RValue<UShort8> lhs, unsigned char rhs)
	{
		return RValue<UShort8>(Nucleus::createShl(lhs.value, C(::context->getConstantInt32(rhs))));
	}

	RValue<UShort8> operator>>(RValue<UShort8> lhs, unsigned char rhs)
	{
		return RValue<UShort8>(Nucleus::createLShr(lhs.value, C(::context->getConstantInt32(rhs))));
	}

	RValue<UShort8> operator+(RValue<UShort8> lhs, RValue<UShort8> rhs)
	{
		return RValue<UShort8>(Nucleus::createAdd(lhs.value, rhs.value));
	}

	RValue<UShort8> operator*(RValue<UShort8> lhs, RValue<UShort8> rhs)
	{
		return RValue<UShort8>(Nucleus::createMul(lhs.value, rhs.value));
	}

	RValue<UShort8> operator+=(const UShort8 &lhs, RValue<UShort8> rhs)
	{
		return lhs = lhs + rhs;
	}

	RValue<UShort8> operator~(RValue<UShort8> val)
	{
		return RValue<UShort8>(Nucleus::createNot(val.value));
	}

	RValue<UShort8> Swizzle(RValue<UShort8> x, char select0, char select1, char select2, char select3, char select4, char select5, char select6, char select7)
	{
		assert(false && "UNIMPLEMENTED"); return RValue<UShort8>(V(nullptr));
	}

	RValue<UShort8> MulHigh(RValue<UShort8> x, RValue<UShort8> y)
	{
		assert(false && "UNIMPLEMENTED"); return RValue<UShort8>(V(nullptr));
	}

	// FIXME: Implement as Shuffle(x, y, Select(i0, ..., i16)) and Shuffle(x, y, SELECT_PACK_REPEAT(element))
//	RValue<UShort8> PackRepeat(RValue<Byte16> x, RValue<Byte16> y, int element)
//	{
//		assert(false && "UNIMPLEMENTED"); return RValue<UShort8>(V(nullptr));
//	}

	Type *UShort8::getType()
	{
		return T(Ice::IceType_v8i16);
	}

	Int::Int(Argument<Int> argument)
	{
		storeValue(argument.value);
	}

	Int::Int(RValue<Byte> cast)
	{
		Value *integer = Nucleus::createZExt(cast.value, Int::getType());

		storeValue(integer);
	}

	Int::Int(RValue<SByte> cast)
	{
		Value *integer = Nucleus::createSExt(cast.value, Int::getType());

		storeValue(integer);
	}

	Int::Int(RValue<Short> cast)
	{
		Value *integer = Nucleus::createSExt(cast.value, Int::getType());

		storeValue(integer);
	}

	Int::Int(RValue<UShort> cast)
	{
		Value *integer = Nucleus::createZExt(cast.value, Int::getType());

		storeValue(integer);
	}

	Int::Int(RValue<Int2> cast)
	{
		*this = Extract(cast, 0);
	}

	Int::Int(RValue<Long> cast)
	{
		Value *integer = Nucleus::createTrunc(cast.value, Int::getType());

		storeValue(integer);
	}

	Int::Int(RValue<Float> cast)
	{
		Value *integer = Nucleus::createFPToSI(cast.value, Int::getType());

		storeValue(integer);
	}

	Int::Int()
	{
	}

	Int::Int(int x)
	{
		storeValue(Nucleus::createConstantInt(x));
	}

	Int::Int(RValue<Int> rhs)
	{
		storeValue(rhs.value);
	}

	Int::Int(RValue<UInt> rhs)
	{
		storeValue(rhs.value);
	}

	Int::Int(const Int &rhs)
	{
		Value *value = rhs.loadValue();
		storeValue(value);
	}

	Int::Int(const Reference<Int> &rhs)
	{
		Value *value = rhs.loadValue();
		storeValue(value);
	}

	Int::Int(const UInt &rhs)
	{
		Value *value = rhs.loadValue();
		storeValue(value);
	}

	Int::Int(const Reference<UInt> &rhs)
	{
		Value *value = rhs.loadValue();
		storeValue(value);
	}

	RValue<Int> Int::operator=(int rhs) const
	{
		return RValue<Int>(storeValue(Nucleus::createConstantInt(rhs)));
	}

	RValue<Int> Int::operator=(RValue<Int> rhs) const
	{
		storeValue(rhs.value);

		return rhs;
	}

	RValue<Int> Int::operator=(RValue<UInt> rhs) const
	{
		storeValue(rhs.value);

		return RValue<Int>(rhs);
	}

	RValue<Int> Int::operator=(const Int &rhs) const
	{
		Value *value = rhs.loadValue();
		storeValue(value);

		return RValue<Int>(value);
	}

	RValue<Int> Int::operator=(const Reference<Int> &rhs) const
	{
		Value *value = rhs.loadValue();
		storeValue(value);

		return RValue<Int>(value);
	}

	RValue<Int> Int::operator=(const UInt &rhs) const
	{
		Value *value = rhs.loadValue();
		storeValue(value);

		return RValue<Int>(value);
	}

	RValue<Int> Int::operator=(const Reference<UInt> &rhs) const
	{
		Value *value = rhs.loadValue();
		storeValue(value);

		return RValue<Int>(value);
	}

	RValue<Int> operator+(RValue<Int> lhs, RValue<Int> rhs)
	{
		return RValue<Int>(Nucleus::createAdd(lhs.value, rhs.value));
	}

	RValue<Int> operator-(RValue<Int> lhs, RValue<Int> rhs)
	{
		return RValue<Int>(Nucleus::createSub(lhs.value, rhs.value));
	}

	RValue<Int> operator*(RValue<Int> lhs, RValue<Int> rhs)
	{
		return RValue<Int>(Nucleus::createMul(lhs.value, rhs.value));
	}

	RValue<Int> operator/(RValue<Int> lhs, RValue<Int> rhs)
	{
		return RValue<Int>(Nucleus::createSDiv(lhs.value, rhs.value));
	}

	RValue<Int> operator%(RValue<Int> lhs, RValue<Int> rhs)
	{
		return RValue<Int>(Nucleus::createSRem(lhs.value, rhs.value));
	}

	RValue<Int> operator&(RValue<Int> lhs, RValue<Int> rhs)
	{
		return RValue<Int>(Nucleus::createAnd(lhs.value, rhs.value));
	}

	RValue<Int> operator|(RValue<Int> lhs, RValue<Int> rhs)
	{
		return RValue<Int>(Nucleus::createOr(lhs.value, rhs.value));
	}

	RValue<Int> operator^(RValue<Int> lhs, RValue<Int> rhs)
	{
		return RValue<Int>(Nucleus::createXor(lhs.value, rhs.value));
	}

	RValue<Int> operator<<(RValue<Int> lhs, RValue<Int> rhs)
	{
		return RValue<Int>(Nucleus::createShl(lhs.value, rhs.value));
	}

	RValue<Int> operator>>(RValue<Int> lhs, RValue<Int> rhs)
	{
		return RValue<Int>(Nucleus::createAShr(lhs.value, rhs.value));
	}

	RValue<Int> operator+=(const Int &lhs, RValue<Int> rhs)
	{
		return lhs = lhs + rhs;
	}

	RValue<Int> operator-=(const Int &lhs, RValue<Int> rhs)
	{
		return lhs = lhs - rhs;
	}

	RValue<Int> operator*=(const Int &lhs, RValue<Int> rhs)
	{
		return lhs = lhs * rhs;
	}

	RValue<Int> operator/=(const Int &lhs, RValue<Int> rhs)
	{
		return lhs = lhs / rhs;
	}

	RValue<Int> operator%=(const Int &lhs, RValue<Int> rhs)
	{
		return lhs = lhs % rhs;
	}

	RValue<Int> operator&=(const Int &lhs, RValue<Int> rhs)
	{
		return lhs = lhs & rhs;
	}

	RValue<Int> operator|=(const Int &lhs, RValue<Int> rhs)
	{
		return lhs = lhs | rhs;
	}

	RValue<Int> operator^=(const Int &lhs, RValue<Int> rhs)
	{
		return lhs = lhs ^ rhs;
	}

	RValue<Int> operator<<=(const Int &lhs, RValue<Int> rhs)
	{
		return lhs = lhs << rhs;
	}

	RValue<Int> operator>>=(const Int &lhs, RValue<Int> rhs)
	{
		return lhs = lhs >> rhs;
	}

	RValue<Int> operator+(RValue<Int> val)
	{
		return val;
	}

	RValue<Int> operator-(RValue<Int> val)
	{
		return RValue<Int>(Nucleus::createNeg(val.value));
	}

	RValue<Int> operator~(RValue<Int> val)
	{
		return RValue<Int>(Nucleus::createNot(val.value));
	}

	RValue<Int> operator++(const Int &val, int)   // Post-increment
	{
		auto oldValue = val.loadValue();
		auto newValue = ::function->makeVariable(Ice::IceType_i32);
		auto inc = Ice::InstArithmetic::create(::function, Ice::InstArithmetic::Add, newValue, oldValue, ::context->getConstantInt32(1));
		::basicBlock->appendInst(inc);
		val.storeValue(V(newValue));

		return RValue<Int>(oldValue);
	}

	const Int &operator++(const Int &val)   // Pre-increment
	{
		assert(false && "UNIMPLEMENTED"); return val;
	}

	RValue<Int> operator--(const Int &val, int)   // Post-decrement
	{
		assert(false && "UNIMPLEMENTED"); return RValue<Int>(V(nullptr));
	}

	const Int &operator--(const Int &val)   // Pre-decrement
	{
		assert(false && "UNIMPLEMENTED"); return val;
	}

	RValue<Bool> operator<(RValue<Int> lhs, RValue<Int> rhs)
	{
		return RValue<Bool>(Nucleus::createICmpSLT(lhs.value, rhs.value));
	}

	RValue<Bool> operator<=(RValue<Int> lhs, RValue<Int> rhs)
	{
		return RValue<Bool>(Nucleus::createICmpSLE(lhs.value, rhs.value));
	}

	RValue<Bool> operator>(RValue<Int> lhs, RValue<Int> rhs)
	{
		return RValue<Bool>(Nucleus::createICmpSGT(lhs.value, rhs.value));
	}

	RValue<Bool> operator>=(RValue<Int> lhs, RValue<Int> rhs)
	{
		return RValue<Bool>(Nucleus::createICmpSGE(lhs.value, rhs.value));
	}

	RValue<Bool> operator!=(RValue<Int> lhs, RValue<Int> rhs)
	{
		return RValue<Bool>(Nucleus::createICmpNE(lhs.value, rhs.value));
	}

	RValue<Bool> operator==(RValue<Int> lhs, RValue<Int> rhs)
	{
		return RValue<Bool>(Nucleus::createICmpEQ(lhs.value, rhs.value));
	}

	RValue<Int> Max(RValue<Int> x, RValue<Int> y)
	{
		return IfThenElse(x > y, x, y);
	}

	RValue<Int> Min(RValue<Int> x, RValue<Int> y)
	{
		return IfThenElse(x < y, x, y);
	}

	RValue<Int> Clamp(RValue<Int> x, RValue<Int> min, RValue<Int> max)
	{
		return Min(Max(x, min), max);
	}

	RValue<Int> RoundInt(RValue<Float> cast)
	{
		assert(false && "UNIMPLEMENTED"); return RValue<Int>(V(nullptr));
	}

	Type *Int::getType()
	{
		return T(Ice::IceType_i32);
	}

	Long::Long(RValue<Int> cast)
	{
		Value *integer = Nucleus::createSExt(cast.value, Long::getType());

		storeValue(integer);
	}

	Long::Long(RValue<UInt> cast)
	{
		Value *integer = Nucleus::createZExt(cast.value, Long::getType());

		storeValue(integer);
	}

	Long::Long()
	{
	}

	Long::Long(RValue<Long> rhs)
	{
		storeValue(rhs.value);
	}

	RValue<Long> Long::operator=(int64_t rhs) const
	{
		return RValue<Long>(storeValue(Nucleus::createConstantLong(rhs)));
	}

	RValue<Long> Long::operator=(RValue<Long> rhs) const
	{
		storeValue(rhs.value);

		return rhs;
	}

	RValue<Long> Long::operator=(const Long &rhs) const
	{
		Value *value = rhs.loadValue();
		storeValue(value);

		return RValue<Long>(value);
	}

	RValue<Long> Long::operator=(const Reference<Long> &rhs) const
	{
		Value *value = rhs.loadValue();
		storeValue(value);

		return RValue<Long>(value);
	}

	RValue<Long> operator+(RValue<Long> lhs, RValue<Long> rhs)
	{
		return RValue<Long>(Nucleus::createAdd(lhs.value, rhs.value));
	}

	RValue<Long> operator-(RValue<Long> lhs, RValue<Long> rhs)
	{
		return RValue<Long>(Nucleus::createSub(lhs.value, rhs.value));
	}

	RValue<Long> operator+=(const Long &lhs, RValue<Long> rhs)
	{
		return lhs = lhs + rhs;
	}

	RValue<Long> operator-=(const Long &lhs, RValue<Long> rhs)
	{
		return lhs = lhs - rhs;
	}

	RValue<Long> AddAtomic(RValue<Pointer<Long> > x, RValue<Long> y)
	{
		return RValue<Long>(Nucleus::createAtomicAdd(x.value, y.value));
	}

	Type *Long::getType()
	{
		return T(Ice::IceType_i64);
	}

	Long1::Long1(const RValue<UInt> cast)
	{
		assert(false && "UNIMPLEMENTED");
	}

	Long1::Long1(RValue<Long1> rhs)
	{
		storeValue(rhs.value);
	}

	Type *Long1::getType()
	{
		assert(false && "UNIMPLEMENTED"); return nullptr;
	}

	UInt::UInt(Argument<UInt> argument)
	{
		storeValue(argument.value);
	}

	UInt::UInt(RValue<UShort> cast)
	{
		Value *integer = Nucleus::createZExt(cast.value, UInt::getType());

		storeValue(integer);
	}

	UInt::UInt(RValue<Long> cast)
	{
		Value *integer = Nucleus::createTrunc(cast.value, UInt::getType());

		storeValue(integer);
	}

	UInt::UInt(RValue<Float> cast)
	{
		assert(false && "UNIMPLEMENTED");
	}

	UInt::UInt()
	{
	}

	UInt::UInt(int x)
	{
		storeValue(Nucleus::createConstantInt(x));
	}

	UInt::UInt(unsigned int x)
	{
		storeValue(Nucleus::createConstantInt(x));
	}

	UInt::UInt(RValue<UInt> rhs)
	{
		storeValue(rhs.value);
	}

	UInt::UInt(RValue<Int> rhs)
	{
		storeValue(rhs.value);
	}

	UInt::UInt(const UInt &rhs)
	{
		Value *value = rhs.loadValue();
		storeValue(value);
	}

	UInt::UInt(const Reference<UInt> &rhs)
	{
		Value *value = rhs.loadValue();
		storeValue(value);
	}

	UInt::UInt(const Int &rhs)
	{
		Value *value = rhs.loadValue();
		storeValue(value);
	}

	UInt::UInt(const Reference<Int> &rhs)
	{
		Value *value = rhs.loadValue();
		storeValue(value);
	}

	RValue<UInt> UInt::operator=(unsigned int rhs) const
	{
		return RValue<UInt>(storeValue(Nucleus::createConstantInt(rhs)));
	}

	RValue<UInt> UInt::operator=(RValue<UInt> rhs) const
	{
		storeValue(rhs.value);

		return rhs;
	}

	RValue<UInt> UInt::operator=(RValue<Int> rhs) const
	{
		storeValue(rhs.value);

		return RValue<UInt>(rhs);
	}

	RValue<UInt> UInt::operator=(const UInt &rhs) const
	{
		Value *value = rhs.loadValue();
		storeValue(value);

		return RValue<UInt>(value);
	}

	RValue<UInt> UInt::operator=(const Reference<UInt> &rhs) const
	{
		Value *value = rhs.loadValue();
		storeValue(value);

		return RValue<UInt>(value);
	}

	RValue<UInt> UInt::operator=(const Int &rhs) const
	{
		Value *value = rhs.loadValue();
		storeValue(value);

		return RValue<UInt>(value);
	}

	RValue<UInt> UInt::operator=(const Reference<Int> &rhs) const
	{
		Value *value = rhs.loadValue();
		storeValue(value);

		return RValue<UInt>(value);
	}

	RValue<UInt> operator+(RValue<UInt> lhs, RValue<UInt> rhs)
	{
		return RValue<UInt>(Nucleus::createAdd(lhs.value, rhs.value));
	}

	RValue<UInt> operator-(RValue<UInt> lhs, RValue<UInt> rhs)
	{
		return RValue<UInt>(Nucleus::createSub(lhs.value, rhs.value));
	}

	RValue<UInt> operator*(RValue<UInt> lhs, RValue<UInt> rhs)
	{
		return RValue<UInt>(Nucleus::createMul(lhs.value, rhs.value));
	}

	RValue<UInt> operator/(RValue<UInt> lhs, RValue<UInt> rhs)
	{
		return RValue<UInt>(Nucleus::createUDiv(lhs.value, rhs.value));
	}

	RValue<UInt> operator%(RValue<UInt> lhs, RValue<UInt> rhs)
	{
		return RValue<UInt>(Nucleus::createURem(lhs.value, rhs.value));
	}

	RValue<UInt> operator&(RValue<UInt> lhs, RValue<UInt> rhs)
	{
		return RValue<UInt>(Nucleus::createAnd(lhs.value, rhs.value));
	}

	RValue<UInt> operator|(RValue<UInt> lhs, RValue<UInt> rhs)
	{
		return RValue<UInt>(Nucleus::createOr(lhs.value, rhs.value));
	}

	RValue<UInt> operator^(RValue<UInt> lhs, RValue<UInt> rhs)
	{
		return RValue<UInt>(Nucleus::createXor(lhs.value, rhs.value));
	}

	RValue<UInt> operator<<(RValue<UInt> lhs, RValue<UInt> rhs)
	{
		return RValue<UInt>(Nucleus::createShl(lhs.value, rhs.value));
	}

	RValue<UInt> operator>>(RValue<UInt> lhs, RValue<UInt> rhs)
	{
		return RValue<UInt>(Nucleus::createLShr(lhs.value, rhs.value));
	}

	RValue<UInt> operator+=(const UInt &lhs, RValue<UInt> rhs)
	{
		return lhs = lhs + rhs;
	}

	RValue<UInt> operator-=(const UInt &lhs, RValue<UInt> rhs)
	{
		return lhs = lhs - rhs;
	}

	RValue<UInt> operator*=(const UInt &lhs, RValue<UInt> rhs)
	{
		return lhs = lhs * rhs;
	}

	RValue<UInt> operator/=(const UInt &lhs, RValue<UInt> rhs)
	{
		return lhs = lhs / rhs;
	}

	RValue<UInt> operator%=(const UInt &lhs, RValue<UInt> rhs)
	{
		return lhs = lhs % rhs;
	}

	RValue<UInt> operator&=(const UInt &lhs, RValue<UInt> rhs)
	{
		return lhs = lhs & rhs;
	}

	RValue<UInt> operator|=(const UInt &lhs, RValue<UInt> rhs)
	{
		return lhs = lhs | rhs;
	}

	RValue<UInt> operator^=(const UInt &lhs, RValue<UInt> rhs)
	{
		return lhs = lhs ^ rhs;
	}

	RValue<UInt> operator<<=(const UInt &lhs, RValue<UInt> rhs)
	{
		return lhs = lhs << rhs;
	}

	RValue<UInt> operator>>=(const UInt &lhs, RValue<UInt> rhs)
	{
		return lhs = lhs >> rhs;
	}

	RValue<UInt> operator+(RValue<UInt> val)
	{
		return val;
	}

	RValue<UInt> operator-(RValue<UInt> val)
	{
		return RValue<UInt>(Nucleus::createNeg(val.value));
	}

	RValue<UInt> operator~(RValue<UInt> val)
	{
		return RValue<UInt>(Nucleus::createNot(val.value));
	}

	RValue<UInt> operator++(const UInt &val, int)   // Post-increment
	{
		assert(false && "UNIMPLEMENTED"); return RValue<UInt>(V(nullptr));
	}

	const UInt &operator++(const UInt &val)   // Pre-increment
	{
		assert(false && "UNIMPLEMENTED"); return val;
	}

	RValue<UInt> operator--(const UInt &val, int)   // Post-decrement
	{
		assert(false && "UNIMPLEMENTED"); return RValue<UInt>(V(nullptr));
	}

	const UInt &operator--(const UInt &val)   // Pre-decrement
	{
		assert(false && "UNIMPLEMENTED"); return val;
	}

	RValue<UInt> Max(RValue<UInt> x, RValue<UInt> y)
	{
		return IfThenElse(x > y, x, y);
	}

	RValue<UInt> Min(RValue<UInt> x, RValue<UInt> y)
	{
		return IfThenElse(x < y, x, y);
	}

	RValue<UInt> Clamp(RValue<UInt> x, RValue<UInt> min, RValue<UInt> max)
	{
		return Min(Max(x, min), max);
	}

	RValue<Bool> operator<(RValue<UInt> lhs, RValue<UInt> rhs)
	{
		return RValue<Bool>(Nucleus::createICmpULT(lhs.value, rhs.value));
	}

	RValue<Bool> operator<=(RValue<UInt> lhs, RValue<UInt> rhs)
	{
		return RValue<Bool>(Nucleus::createICmpULE(lhs.value, rhs.value));
	}

	RValue<Bool> operator>(RValue<UInt> lhs, RValue<UInt> rhs)
	{
		return RValue<Bool>(Nucleus::createICmpUGT(lhs.value, rhs.value));
	}

	RValue<Bool> operator>=(RValue<UInt> lhs, RValue<UInt> rhs)
	{
		return RValue<Bool>(Nucleus::createICmpUGE(lhs.value, rhs.value));
	}

	RValue<Bool> operator!=(RValue<UInt> lhs, RValue<UInt> rhs)
	{
		return RValue<Bool>(Nucleus::createICmpNE(lhs.value, rhs.value));
	}

	RValue<Bool> operator==(RValue<UInt> lhs, RValue<UInt> rhs)
	{
		return RValue<Bool>(Nucleus::createICmpEQ(lhs.value, rhs.value));
	}

//	RValue<UInt> RoundUInt(RValue<Float> cast)
//	{
//		assert(false && "UNIMPLEMENTED"); return RValue<UInt>(V(nullptr));
//	}

	Type *UInt::getType()
	{
		return T(Ice::IceType_i32);
	}

//	Int2::Int2(RValue<Int> cast)
//	{
//		Value *extend = Nucleus::createZExt(cast.value, Long::getType());
//		Value *vector = Nucleus::createBitCast(extend, Int2::getType());
//
//		Constant *shuffle[2];
//		shuffle[0] = Nucleus::createConstantInt(0);
//		shuffle[1] = Nucleus::createConstantInt(0);
//
//		Value *replicate = Nucleus::createShuffleVector(vector, UndefValue::get(Int2::getType()), Nucleus::createConstantVector(shuffle, 2));
//
//		storeValue(replicate);
//	}

	Int2::Int2(RValue<Int4> cast)
	{
		storeValue(Nucleus::createBitCast(cast.value, getType()));
	}

	Int2::Int2()
	{
	//	xy.parent = this;
	}

	Int2::Int2(int x, int y)
	{
	//	xy.parent = this;

		int64_t constantVector[2] = {x, y};
		storeValue(Nucleus::createConstantVector(constantVector, getType()));
	}

	Int2::Int2(RValue<Int2> rhs)
	{
	//	xy.parent = this;

		storeValue(rhs.value);
	}

	Int2::Int2(const Int2 &rhs)
	{
	//	xy.parent = this;

		Value *value = rhs.loadValue();
		storeValue(value);
	}

	Int2::Int2(const Reference<Int2> &rhs)
	{
	//	xy.parent = this;

		Value *value = rhs.loadValue();
		storeValue(value);
	}

	Int2::Int2(RValue<Int> lo, RValue<Int> hi)
	{
		assert(false && "UNIMPLEMENTED");
	}

	RValue<Int2> Int2::operator=(RValue<Int2> rhs) const
	{
		storeValue(rhs.value);

		return rhs;
	}

	RValue<Int2> Int2::operator=(const Int2 &rhs) const
	{
		Value *value = rhs.loadValue();
		storeValue(value);

		return RValue<Int2>(value);
	}

	RValue<Int2> Int2::operator=(const Reference<Int2> &rhs) const
	{
		Value *value = rhs.loadValue();
		storeValue(value);

		return RValue<Int2>(value);
	}

	RValue<Int2> operator+(RValue<Int2> lhs, RValue<Int2> rhs)
	{
		return RValue<Int2>(Nucleus::createAdd(lhs.value, rhs.value));
	}

	RValue<Int2> operator-(RValue<Int2> lhs, RValue<Int2> rhs)
	{
		return RValue<Int2>(Nucleus::createSub(lhs.value, rhs.value));
	}

//	RValue<Int2> operator*(RValue<Int2> lhs, RValue<Int2> rhs)
//	{
//		return RValue<Int2>(Nucleus::createMul(lhs.value, rhs.value));
//	}

//	RValue<Int2> operator/(RValue<Int2> lhs, RValue<Int2> rhs)
//	{
//		return RValue<Int2>(Nucleus::createSDiv(lhs.value, rhs.value));
//	}

//	RValue<Int2> operator%(RValue<Int2> lhs, RValue<Int2> rhs)
//	{
//		return RValue<Int2>(Nucleus::createSRem(lhs.value, rhs.value));
//	}

	RValue<Int2> operator&(RValue<Int2> lhs, RValue<Int2> rhs)
	{
		return RValue<Int2>(Nucleus::createAnd(lhs.value, rhs.value));
	}

	RValue<Int2> operator|(RValue<Int2> lhs, RValue<Int2> rhs)
	{
		return RValue<Int2>(Nucleus::createOr(lhs.value, rhs.value));
	}

	RValue<Int2> operator^(RValue<Int2> lhs, RValue<Int2> rhs)
	{
		return RValue<Int2>(Nucleus::createXor(lhs.value, rhs.value));
	}

	RValue<Int2> operator<<(RValue<Int2> lhs, unsigned char rhs)
	{
		return RValue<Int2>(Nucleus::createShl(lhs.value, C(::context->getConstantInt32(rhs))));
	}

	RValue<Int2> operator>>(RValue<Int2> lhs, unsigned char rhs)
	{
		return RValue<Int2>(Nucleus::createAShr(lhs.value, C(::context->getConstantInt32(rhs))));
	}

	RValue<Int2> operator<<(RValue<Int2> lhs, RValue<Long1> rhs)
	{
		assert(false && "UNIMPLEMENTED"); return RValue<Int2>(V(nullptr));
	}

	RValue<Int2> operator>>(RValue<Int2> lhs, RValue<Long1> rhs)
	{
		assert(false && "UNIMPLEMENTED"); return RValue<Int2>(V(nullptr));
	}

	RValue<Int2> operator+=(const Int2 &lhs, RValue<Int2> rhs)
	{
		return lhs = lhs + rhs;
	}

	RValue<Int2> operator-=(const Int2 &lhs, RValue<Int2> rhs)
	{
		return lhs = lhs - rhs;
	}

//	RValue<Int2> operator*=(const Int2 &lhs, RValue<Int2> rhs)
//	{
//		return lhs = lhs * rhs;
//	}

//	RValue<Int2> operator/=(const Int2 &lhs, RValue<Int2> rhs)
//	{
//		return lhs = lhs / rhs;
//	}

//	RValue<Int2> operator%=(const Int2 &lhs, RValue<Int2> rhs)
//	{
//		return lhs = lhs % rhs;
//	}

	RValue<Int2> operator&=(const Int2 &lhs, RValue<Int2> rhs)
	{
		return lhs = lhs & rhs;
	}

	RValue<Int2> operator|=(const Int2 &lhs, RValue<Int2> rhs)
	{
		return lhs = lhs | rhs;
	}

	RValue<Int2> operator^=(const Int2 &lhs, RValue<Int2> rhs)
	{
		return lhs = lhs ^ rhs;
	}

	RValue<Int2> operator<<=(const Int2 &lhs, unsigned char rhs)
	{
		return lhs = lhs << rhs;
	}

	RValue<Int2> operator>>=(const Int2 &lhs, unsigned char rhs)
	{
		return lhs = lhs >> rhs;
	}

	RValue<Int2> operator<<=(const Int2 &lhs, RValue<Long1> rhs)
	{
		return lhs = lhs << rhs;
	}

	RValue<Int2> operator>>=(const Int2 &lhs, RValue<Long1> rhs)
	{
		return lhs = lhs >> rhs;
	}

//	RValue<Int2> operator+(RValue<Int2> val)
//	{
//		return val;
//	}

//	RValue<Int2> operator-(RValue<Int2> val)
//	{
//		return RValue<Int2>(Nucleus::createNeg(val.value));
//	}

	RValue<Int2> operator~(RValue<Int2> val)
	{
		assert(false && "UNIMPLEMENTED"); return RValue<Int2>(V(nullptr));
	}

	RValue<Long1> UnpackLow(RValue<Int2> x, RValue<Int2> y)
	{
		assert(false && "UNIMPLEMENTED"); return RValue<Long1>(V(nullptr));
	}

	RValue<Long1> UnpackHigh(RValue<Int2> x, RValue<Int2> y)
	{
		assert(false && "UNIMPLEMENTED"); return RValue<Long1>(V(nullptr));
	}

	RValue<Int> Extract(RValue<Int2> val, int i)
	{
		assert(false && "UNIMPLEMENTED"); return RValue<Int>(V(nullptr));
	}

	RValue<Int2> Insert(RValue<Int2> val, RValue<Int> element, int i)
	{
		assert(false && "UNIMPLEMENTED"); return RValue<Int2>(V(nullptr));
	}

	Type *Int2::getType()
	{
		return T(Type_v2i32);
	}

	UInt2::UInt2()
	{
	//	xy.parent = this;
	}

	UInt2::UInt2(unsigned int x, unsigned int y)
	{
	//	xy.parent = this;

		int64_t constantVector[2] = {x, y};
		storeValue(Nucleus::createConstantVector(constantVector, getType()));
	}

	UInt2::UInt2(RValue<UInt2> rhs)
	{
	//	xy.parent = this;

		storeValue(rhs.value);
	}

	UInt2::UInt2(const UInt2 &rhs)
	{
	//	xy.parent = this;

		Value *value = rhs.loadValue();
		storeValue(value);
	}

	UInt2::UInt2(const Reference<UInt2> &rhs)
	{
	//	xy.parent = this;

		Value *value = rhs.loadValue();
		storeValue(value);
	}

	RValue<UInt2> UInt2::operator=(RValue<UInt2> rhs) const
	{
		storeValue(rhs.value);

		return rhs;
	}

	RValue<UInt2> UInt2::operator=(const UInt2 &rhs) const
	{
		Value *value = rhs.loadValue();
		storeValue(value);

		return RValue<UInt2>(value);
	}

	RValue<UInt2> UInt2::operator=(const Reference<UInt2> &rhs) const
	{
		Value *value = rhs.loadValue();
		storeValue(value);

		return RValue<UInt2>(value);
	}

	RValue<UInt2> operator+(RValue<UInt2> lhs, RValue<UInt2> rhs)
	{
		return RValue<UInt2>(Nucleus::createAdd(lhs.value, rhs.value));
	}

	RValue<UInt2> operator-(RValue<UInt2> lhs, RValue<UInt2> rhs)
	{
		return RValue<UInt2>(Nucleus::createSub(lhs.value, rhs.value));
	}

//	RValue<UInt2> operator*(RValue<UInt2> lhs, RValue<UInt2> rhs)
//	{
//		return RValue<UInt2>(Nucleus::createMul(lhs.value, rhs.value));
//	}

//	RValue<UInt2> operator/(RValue<UInt2> lhs, RValue<UInt2> rhs)
//	{
//		return RValue<UInt2>(Nucleus::createUDiv(lhs.value, rhs.value));
//	}

//	RValue<UInt2> operator%(RValue<UInt2> lhs, RValue<UInt2> rhs)
//	{
//		return RValue<UInt2>(Nucleus::createURem(lhs.value, rhs.value));
//	}

	RValue<UInt2> operator&(RValue<UInt2> lhs, RValue<UInt2> rhs)
	{
		return RValue<UInt2>(Nucleus::createAnd(lhs.value, rhs.value));
	}

	RValue<UInt2> operator|(RValue<UInt2> lhs, RValue<UInt2> rhs)
	{
		return RValue<UInt2>(Nucleus::createOr(lhs.value, rhs.value));
	}

	RValue<UInt2> operator^(RValue<UInt2> lhs, RValue<UInt2> rhs)
	{
		return RValue<UInt2>(Nucleus::createXor(lhs.value, rhs.value));
	}

	RValue<UInt2> operator<<(RValue<UInt2> lhs, unsigned char rhs)
	{
		return RValue<UInt2>(Nucleus::createShl(lhs.value, C(::context->getConstantInt32(rhs))));
	}

	RValue<UInt2> operator>>(RValue<UInt2> lhs, unsigned char rhs)
	{
		return RValue<UInt2>(Nucleus::createLShr(lhs.value, C(::context->getConstantInt32(rhs))));
	}

	RValue<UInt2> operator<<(RValue<UInt2> lhs, RValue<Long1> rhs)
	{
		assert(false && "UNIMPLEMENTED"); return RValue<UInt2>(V(nullptr));
	}

	RValue<UInt2> operator>>(RValue<UInt2> lhs, RValue<Long1> rhs)
	{
		assert(false && "UNIMPLEMENTED"); return RValue<UInt2>(V(nullptr));
	}

	RValue<UInt2> operator+=(const UInt2 &lhs, RValue<UInt2> rhs)
	{
		return lhs = lhs + rhs;
	}

	RValue<UInt2> operator-=(const UInt2 &lhs, RValue<UInt2> rhs)
	{
		return lhs = lhs - rhs;
	}

//	RValue<UInt2> operator*=(const UInt2 &lhs, RValue<UInt2> rhs)
//	{
//		return lhs = lhs * rhs;
//	}

//	RValue<UInt2> operator/=(const UInt2 &lhs, RValue<UInt2> rhs)
//	{
//		return lhs = lhs / rhs;
//	}

//	RValue<UInt2> operator%=(const UInt2 &lhs, RValue<UInt2> rhs)
//	{
//		return lhs = lhs % rhs;
//	}

	RValue<UInt2> operator&=(const UInt2 &lhs, RValue<UInt2> rhs)
	{
		return lhs = lhs & rhs;
	}

	RValue<UInt2> operator|=(const UInt2 &lhs, RValue<UInt2> rhs)
	{
		return lhs = lhs | rhs;
	}

	RValue<UInt2> operator^=(const UInt2 &lhs, RValue<UInt2> rhs)
	{
		return lhs = lhs ^ rhs;
	}

	RValue<UInt2> operator<<=(const UInt2 &lhs, unsigned char rhs)
	{
		return lhs = lhs << rhs;
	}

	RValue<UInt2> operator>>=(const UInt2 &lhs, unsigned char rhs)
	{
		return lhs = lhs >> rhs;
	}

	RValue<UInt2> operator<<=(const UInt2 &lhs, RValue<Long1> rhs)
	{
		return lhs = lhs << rhs;
	}

	RValue<UInt2> operator>>=(const UInt2 &lhs, RValue<Long1> rhs)
	{
		return lhs = lhs >> rhs;
	}

//	RValue<UInt2> operator+(RValue<UInt2> val)
//	{
//		return val;
//	}

//	RValue<UInt2> operator-(RValue<UInt2> val)
//	{
//		return RValue<UInt2>(Nucleus::createNeg(val.value));
//	}

	RValue<UInt2> operator~(RValue<UInt2> val)
	{
		return RValue<UInt2>(Nucleus::createNot(val.value));
	}

	Type *UInt2::getType()
	{
		return T(Type_v2i32);
	}

	Int4::Int4(RValue<Byte4> cast)
	{
		assert(false && "UNIMPLEMENTED");
	}

	Int4::Int4(RValue<SByte4> cast)
	{
		assert(false && "UNIMPLEMENTED");
	}

	Int4::Int4(RValue<Float4> cast)
	{
	//	xyzw.parent = this;

		Value *xyzw = Nucleus::createFPToSI(cast.value, Int4::getType());

		storeValue(xyzw);
	}

	Int4::Int4(RValue<Short4> cast)
	{
		assert(false && "UNIMPLEMENTED");
	}

	Int4::Int4(RValue<UShort4> cast)
	{
		assert(false && "UNIMPLEMENTED");
	}

	Int4::Int4()
	{
	//	xyzw.parent = this;
	}

	Int4::Int4(int xyzw)
	{
		constant(xyzw, xyzw, xyzw, xyzw);
	}

	Int4::Int4(int x, int yzw)
	{
		constant(x, yzw, yzw, yzw);
	}

	Int4::Int4(int x, int y, int zw)
	{
		constant(x, y, zw, zw);
	}

	Int4::Int4(int x, int y, int z, int w)
	{
		constant(x, y, z, w);
	}

	void Int4::constant(int x, int y, int z, int w)
	{
	//	xyzw.parent = this;

		int64_t constantVector[4] = {x, y, z, w};
		storeValue(Nucleus::createConstantVector(constantVector, getType()));
	}

	Int4::Int4(RValue<Int4> rhs)
	{
	//	xyzw.parent = this;

		storeValue(rhs.value);
	}

	Int4::Int4(const Int4 &rhs)
	{
	//	xyzw.parent = this;

		Value *value = rhs.loadValue();
		storeValue(value);
	}

	Int4::Int4(const Reference<Int4> &rhs)
	{
	//	xyzw.parent = this;

		Value *value = rhs.loadValue();
		storeValue(value);
	}

	Int4::Int4(RValue<UInt4> rhs)
	{
	//	xyzw.parent = this;

		storeValue(rhs.value);
	}

	Int4::Int4(const UInt4 &rhs)
	{
	//	xyzw.parent = this;

		Value *value = rhs.loadValue();
		storeValue(value);
	}

	Int4::Int4(const Reference<UInt4> &rhs)
	{
	//	xyzw.parent = this;

		Value *value = rhs.loadValue();
		storeValue(value);
	}

	Int4::Int4(RValue<Int2> lo, RValue<Int2> hi)
	{
		assert(false && "UNIMPLEMENTED");
	}

	Int4::Int4(RValue<Int> rhs)
	{
	//	xyzw.parent = this;

		assert(false && "UNIMPLEMENTED");
	}

	Int4::Int4(const Int &rhs)
	{
	//	xyzw.parent = this;

		*this = RValue<Int>(rhs.loadValue());
	}

	Int4::Int4(const Reference<Int> &rhs)
	{
	//	xyzw.parent = this;

		*this = RValue<Int>(rhs.loadValue());
	}

	RValue<Int4> Int4::operator=(RValue<Int4> rhs) const
	{
		storeValue(rhs.value);

		return rhs;
	}

	RValue<Int4> Int4::operator=(const Int4 &rhs) const
	{
		Value *value = rhs.loadValue();
		storeValue(value);

		return RValue<Int4>(value);
	}

	RValue<Int4> Int4::operator=(const Reference<Int4> &rhs) const
	{
		Value *value = rhs.loadValue();
		storeValue(value);

		return RValue<Int4>(value);
	}

	RValue<Int4> operator+(RValue<Int4> lhs, RValue<Int4> rhs)
	{
		return RValue<Int4>(Nucleus::createAdd(lhs.value, rhs.value));
	}

	RValue<Int4> operator-(RValue<Int4> lhs, RValue<Int4> rhs)
	{
		return RValue<Int4>(Nucleus::createSub(lhs.value, rhs.value));
	}

	RValue<Int4> operator*(RValue<Int4> lhs, RValue<Int4> rhs)
	{
		return RValue<Int4>(Nucleus::createMul(lhs.value, rhs.value));
	}

	RValue<Int4> operator/(RValue<Int4> lhs, RValue<Int4> rhs)
	{
		return RValue<Int4>(Nucleus::createSDiv(lhs.value, rhs.value));
	}

	RValue<Int4> operator%(RValue<Int4> lhs, RValue<Int4> rhs)
	{
		return RValue<Int4>(Nucleus::createSRem(lhs.value, rhs.value));
	}

	RValue<Int4> operator&(RValue<Int4> lhs, RValue<Int4> rhs)
	{
		return RValue<Int4>(Nucleus::createAnd(lhs.value, rhs.value));
	}

	RValue<Int4> operator|(RValue<Int4> lhs, RValue<Int4> rhs)
	{
		return RValue<Int4>(Nucleus::createOr(lhs.value, rhs.value));
	}

	RValue<Int4> operator^(RValue<Int4> lhs, RValue<Int4> rhs)
	{
		return RValue<Int4>(Nucleus::createXor(lhs.value, rhs.value));
	}

	RValue<Int4> operator<<(RValue<Int4> lhs, unsigned char rhs)
	{
		return RValue<Int4>(Nucleus::createShl(lhs.value, C(::context->getConstantInt32(rhs))));
	}

	RValue<Int4> operator>>(RValue<Int4> lhs, unsigned char rhs)
	{
		return RValue<Int4>(Nucleus::createAShr(lhs.value, C(::context->getConstantInt32(rhs))));
	}

	RValue<Int4> operator<<(RValue<Int4> lhs, RValue<Int4> rhs)
	{
		return RValue<Int4>(Nucleus::createShl(lhs.value, rhs.value));
	}

	RValue<Int4> operator>>(RValue<Int4> lhs, RValue<Int4> rhs)
	{
		return RValue<Int4>(Nucleus::createAShr(lhs.value, rhs.value));
	}

	RValue<Int4> operator+=(const Int4 &lhs, RValue<Int4> rhs)
	{
		return lhs = lhs + rhs;
	}

	RValue<Int4> operator-=(const Int4 &lhs, RValue<Int4> rhs)
	{
		return lhs = lhs - rhs;
	}

	RValue<Int4> operator*=(const Int4 &lhs, RValue<Int4> rhs)
	{
		return lhs = lhs * rhs;
	}

//	RValue<Int4> operator/=(const Int4 &lhs, RValue<Int4> rhs)
//	{
//		return lhs = lhs / rhs;
//	}

//	RValue<Int4> operator%=(const Int4 &lhs, RValue<Int4> rhs)
//	{
//		return lhs = lhs % rhs;
//	}

	RValue<Int4> operator&=(const Int4 &lhs, RValue<Int4> rhs)
	{
		return lhs = lhs & rhs;
	}

	RValue<Int4> operator|=(const Int4 &lhs, RValue<Int4> rhs)
	{
		return lhs = lhs | rhs;
	}

	RValue<Int4> operator^=(const Int4 &lhs, RValue<Int4> rhs)
	{
		return lhs = lhs ^ rhs;
	}

	RValue<Int4> operator<<=(const Int4 &lhs, unsigned char rhs)
	{
		return lhs = lhs << rhs;
	}

	RValue<Int4> operator>>=(const Int4 &lhs, unsigned char rhs)
	{
		return lhs = lhs >> rhs;
	}

	RValue<Int4> operator+(RValue<Int4> val)
	{
		return val;
	}

	RValue<Int4> operator-(RValue<Int4> val)
	{
		return RValue<Int4>(Nucleus::createNeg(val.value));
	}

	RValue<Int4> operator~(RValue<Int4> val)
	{
		return RValue<Int4>(Nucleus::createNot(val.value));
	}

	RValue<Int4> CmpEQ(RValue<Int4> x, RValue<Int4> y)
	{
		return RValue<Int4>(Nucleus::createSExt(Nucleus::createICmpEQ(x.value, y.value), Int4::getType()));
	}

	RValue<Int4> CmpLT(RValue<Int4> x, RValue<Int4> y)
	{
		return RValue<Int4>(Nucleus::createSExt(Nucleus::createICmpSLT(x.value, y.value), Int4::getType()));
	}

	RValue<Int4> CmpLE(RValue<Int4> x, RValue<Int4> y)
	{
		return RValue<Int4>(Nucleus::createSExt(Nucleus::createICmpSLE(x.value, y.value), Int4::getType()));
	}

	RValue<Int4> CmpNEQ(RValue<Int4> x, RValue<Int4> y)
	{
		return RValue<Int4>(Nucleus::createSExt(Nucleus::createICmpNE(x.value, y.value), Int4::getType()));
	}

	RValue<Int4> CmpNLT(RValue<Int4> x, RValue<Int4> y)
	{
		return RValue<Int4>(Nucleus::createSExt(Nucleus::createICmpSGE(x.value, y.value), Int4::getType()));
	}

	RValue<Int4> CmpNLE(RValue<Int4> x, RValue<Int4> y)
	{
		return RValue<Int4>(Nucleus::createSExt(Nucleus::createICmpSGT(x.value, y.value), Int4::getType()));
	}

	RValue<Int4> Max(RValue<Int4> x, RValue<Int4> y)
	{
		Ice::Variable *condition = ::function->makeVariable(Ice::IceType_v4i1);
		auto cmp = Ice::InstIcmp::create(::function, Ice::InstIcmp::Sle, condition, x.value, y.value);
		::basicBlock->appendInst(cmp);

		Ice::Variable *result = ::function->makeVariable(Ice::IceType_v4i32);
		auto select = Ice::InstSelect::create(::function, result, condition, y.value, x.value);
		::basicBlock->appendInst(select);

		return RValue<Int4>(V(result));
	}

	RValue<Int4> Min(RValue<Int4> x, RValue<Int4> y)
	{
		Ice::Variable *condition = ::function->makeVariable(Ice::IceType_v4i1);
		auto cmp = Ice::InstIcmp::create(::function, Ice::InstIcmp::Sgt, condition, x.value, y.value);
		::basicBlock->appendInst(cmp);

		Ice::Variable *result = ::function->makeVariable(Ice::IceType_v4i32);
		auto select = Ice::InstSelect::create(::function, result, condition, y.value, x.value);
		::basicBlock->appendInst(select);

		return RValue<Int4>(V(result));
	}

	RValue<Int4> RoundInt(RValue<Float4> cast)
	{
		assert(false && "UNIMPLEMENTED"); return RValue<Int4>(V(nullptr));
	}

	RValue<Short8> Pack(RValue<Int4> x, RValue<Int4> y)
	{
		Ice::Variable *result = ::function->makeVariable(Ice::IceType_v8i16);
		const Ice::Intrinsics::IntrinsicInfo intrinsic = {Ice::Intrinsics::VectorPackSigned, Ice::Intrinsics::SideEffects_F, Ice::Intrinsics::ReturnsTwice_F, Ice::Intrinsics::MemoryWrite_F};
		auto target = ::context->getConstantUndef(Ice::IceType_i32);
		auto pack = Ice::InstIntrinsicCall::create(::function, 2, result, target, intrinsic);
		pack->addArg(x.value);
		pack->addArg(y.value);
		::basicBlock->appendInst(pack);

		return RValue<Short8>(V(result));
	}

	RValue<Int> Extract(RValue<Int4> x, int i)
	{
		return RValue<Int>(Nucleus::createExtractElement(x.value, Int::getType(), i));
	}

	RValue<Int4> Insert(RValue<Int4> x, RValue<Int> element, int i)
	{
		return RValue<Int4>(Nucleus::createInsertElement(x.value, element.value, i));
	}

	RValue<Int> SignMask(RValue<Int4> x)
	{
		Ice::Variable *result = ::function->makeVariable(Ice::IceType_i32);
		const Ice::Intrinsics::IntrinsicInfo intrinsic = {Ice::Intrinsics::SignMask, Ice::Intrinsics::SideEffects_F, Ice::Intrinsics::ReturnsTwice_F, Ice::Intrinsics::MemoryWrite_F};
		auto target = ::context->getConstantUndef(Ice::IceType_i32);
		auto movmsk = Ice::InstIntrinsicCall::create(::function, 1, result, target, intrinsic);
		movmsk->addArg(x.value);
		::basicBlock->appendInst(movmsk);

		return RValue<Int>(V(result));
	}

	RValue<Int4> Swizzle(RValue<Int4> x, unsigned char select)
	{
		return RValue<Int4>(createSwizzle4(x.value, select));
	}

	Type *Int4::getType()
	{
		return T(Ice::IceType_v4i32);
	}

	UInt4::UInt4(RValue<Float4> cast)
	{
	//	xyzw.parent = this;

		assert(false && "UNIMPLEMENTED");
	}

	UInt4::UInt4()
	{
	//	xyzw.parent = this;
	}

	UInt4::UInt4(int xyzw)
	{
		constant(xyzw, xyzw, xyzw, xyzw);
	}

	UInt4::UInt4(int x, int yzw)
	{
		constant(x, yzw, yzw, yzw);
	}

	UInt4::UInt4(int x, int y, int zw)
	{
		constant(x, y, zw, zw);
	}

	UInt4::UInt4(int x, int y, int z, int w)
	{
		constant(x, y, z, w);
	}

	void UInt4::constant(int x, int y, int z, int w)
	{
	//	xyzw.parent = this;

		int64_t constantVector[4] = {x, y, z, w};
		storeValue(Nucleus::createConstantVector(constantVector, getType()));
	}

	UInt4::UInt4(RValue<UInt4> rhs)
	{
	//	xyzw.parent = this;

		storeValue(rhs.value);
	}

	UInt4::UInt4(const UInt4 &rhs)
	{
	//	xyzw.parent = this;

		Value *value = rhs.loadValue();
		storeValue(value);
	}

	UInt4::UInt4(const Reference<UInt4> &rhs)
	{
	//	xyzw.parent = this;

		Value *value = rhs.loadValue();
		storeValue(value);
	}

	UInt4::UInt4(RValue<Int4> rhs)
	{
	//	xyzw.parent = this;

		storeValue(rhs.value);
	}

	UInt4::UInt4(const Int4 &rhs)
	{
	//	xyzw.parent = this;

		Value *value = rhs.loadValue();
		storeValue(value);
	}

	UInt4::UInt4(const Reference<Int4> &rhs)
	{
	//	xyzw.parent = this;

		Value *value = rhs.loadValue();
		storeValue(value);
	}

	UInt4::UInt4(RValue<UInt2> lo, RValue<UInt2> hi)
	{
		assert(false && "UNIMPLEMENTED");
	}

	RValue<UInt4> UInt4::operator=(RValue<UInt4> rhs) const
	{
		storeValue(rhs.value);

		return rhs;
	}

	RValue<UInt4> UInt4::operator=(const UInt4 &rhs) const
	{
		Value *value = rhs.loadValue();
		storeValue(value);

		return RValue<UInt4>(value);
	}

	RValue<UInt4> UInt4::operator=(const Reference<UInt4> &rhs) const
	{
		Value *value = rhs.loadValue();
		storeValue(value);

		return RValue<UInt4>(value);
	}

	RValue<UInt4> operator+(RValue<UInt4> lhs, RValue<UInt4> rhs)
	{
		return RValue<UInt4>(Nucleus::createAdd(lhs.value, rhs.value));
	}

	RValue<UInt4> operator-(RValue<UInt4> lhs, RValue<UInt4> rhs)
	{
		return RValue<UInt4>(Nucleus::createSub(lhs.value, rhs.value));
	}

	RValue<UInt4> operator*(RValue<UInt4> lhs, RValue<UInt4> rhs)
	{
		return RValue<UInt4>(Nucleus::createMul(lhs.value, rhs.value));
	}

	RValue<UInt4> operator/(RValue<UInt4> lhs, RValue<UInt4> rhs)
	{
		return RValue<UInt4>(Nucleus::createUDiv(lhs.value, rhs.value));
	}

	RValue<UInt4> operator%(RValue<UInt4> lhs, RValue<UInt4> rhs)
	{
		return RValue<UInt4>(Nucleus::createURem(lhs.value, rhs.value));
	}

	RValue<UInt4> operator&(RValue<UInt4> lhs, RValue<UInt4> rhs)
	{
		return RValue<UInt4>(Nucleus::createAnd(lhs.value, rhs.value));
	}

	RValue<UInt4> operator|(RValue<UInt4> lhs, RValue<UInt4> rhs)
	{
		return RValue<UInt4>(Nucleus::createOr(lhs.value, rhs.value));
	}

	RValue<UInt4> operator^(RValue<UInt4> lhs, RValue<UInt4> rhs)
	{
		return RValue<UInt4>(Nucleus::createXor(lhs.value, rhs.value));
	}

	RValue<UInt4> operator<<(RValue<UInt4> lhs, unsigned char rhs)
	{
		return RValue<UInt4>(Nucleus::createShl(lhs.value, C(::context->getConstantInt32(rhs))));
	}

	RValue<UInt4> operator>>(RValue<UInt4> lhs, unsigned char rhs)
	{
		return RValue<UInt4>(Nucleus::createLShr(lhs.value, C(::context->getConstantInt32(rhs))));
	}

	RValue<UInt4> operator<<(RValue<UInt4> lhs, RValue<UInt4> rhs)
	{
		return RValue<UInt4>(Nucleus::createShl(lhs.value, rhs.value));
	}

	RValue<UInt4> operator>>(RValue<UInt4> lhs, RValue<UInt4> rhs)
	{
		return RValue<UInt4>(Nucleus::createLShr(lhs.value, rhs.value));
	}

	RValue<UInt4> operator+=(const UInt4 &lhs, RValue<UInt4> rhs)
	{
		return lhs = lhs + rhs;
	}

	RValue<UInt4> operator-=(const UInt4 &lhs, RValue<UInt4> rhs)
	{
		return lhs = lhs - rhs;
	}

	RValue<UInt4> operator*=(const UInt4 &lhs, RValue<UInt4> rhs)
	{
		return lhs = lhs * rhs;
	}

//	RValue<UInt4> operator/=(const UInt4 &lhs, RValue<UInt4> rhs)
//	{
//		return lhs = lhs / rhs;
//	}

//	RValue<UInt4> operator%=(const UInt4 &lhs, RValue<UInt4> rhs)
//	{
//		return lhs = lhs % rhs;
//	}

	RValue<UInt4> operator&=(const UInt4 &lhs, RValue<UInt4> rhs)
	{
		return lhs = lhs & rhs;
	}

	RValue<UInt4> operator|=(const UInt4 &lhs, RValue<UInt4> rhs)
	{
		return lhs = lhs | rhs;
	}

	RValue<UInt4> operator^=(const UInt4 &lhs, RValue<UInt4> rhs)
	{
		return lhs = lhs ^ rhs;
	}

	RValue<UInt4> operator<<=(const UInt4 &lhs, unsigned char rhs)
	{
		return lhs = lhs << rhs;
	}

	RValue<UInt4> operator>>=(const UInt4 &lhs, unsigned char rhs)
	{
		return lhs = lhs >> rhs;
	}

	RValue<UInt4> operator+(RValue<UInt4> val)
	{
		return val;
	}

	RValue<UInt4> operator-(RValue<UInt4> val)
	{
		return RValue<UInt4>(Nucleus::createNeg(val.value));
	}

	RValue<UInt4> operator~(RValue<UInt4> val)
	{
		return RValue<UInt4>(Nucleus::createNot(val.value));
	}

	RValue<UInt4> CmpEQ(RValue<UInt4> x, RValue<UInt4> y)
	{
		return RValue<UInt4>(Nucleus::createSExt(Nucleus::createICmpEQ(x.value, y.value), Int4::getType()));
	}

	RValue<UInt4> CmpLT(RValue<UInt4> x, RValue<UInt4> y)
	{
		return RValue<UInt4>(Nucleus::createSExt(Nucleus::createICmpULT(x.value, y.value), Int4::getType()));
	}

	RValue<UInt4> CmpLE(RValue<UInt4> x, RValue<UInt4> y)
	{
		return RValue<UInt4>(Nucleus::createSExt(Nucleus::createICmpULE(x.value, y.value), Int4::getType()));
	}

	RValue<UInt4> CmpNEQ(RValue<UInt4> x, RValue<UInt4> y)
	{
		return RValue<UInt4>(Nucleus::createSExt(Nucleus::createICmpNE(x.value, y.value), Int4::getType()));
	}

	RValue<UInt4> CmpNLT(RValue<UInt4> x, RValue<UInt4> y)
	{
		return RValue<UInt4>(Nucleus::createSExt(Nucleus::createICmpUGE(x.value, y.value), Int4::getType()));
	}

	RValue<UInt4> CmpNLE(RValue<UInt4> x, RValue<UInt4> y)
	{
		return RValue<UInt4>(Nucleus::createSExt(Nucleus::createICmpUGT(x.value, y.value), Int4::getType()));
	}

	RValue<UInt4> Max(RValue<UInt4> x, RValue<UInt4> y)
	{
		Ice::Variable *condition = ::function->makeVariable(Ice::IceType_v4i1);
		auto cmp = Ice::InstIcmp::create(::function, Ice::InstIcmp::Ule, condition, x.value, y.value);
		::basicBlock->appendInst(cmp);

		Ice::Variable *result = ::function->makeVariable(Ice::IceType_v4i32);
		auto select = Ice::InstSelect::create(::function, result, condition, y.value, x.value);
		::basicBlock->appendInst(select);

		return RValue<UInt4>(V(result));
	}

	RValue<UInt4> Min(RValue<UInt4> x, RValue<UInt4> y)
	{
		Ice::Variable *condition = ::function->makeVariable(Ice::IceType_v4i1);
		auto cmp = Ice::InstIcmp::create(::function, Ice::InstIcmp::Ugt, condition, x.value, y.value);
		::basicBlock->appendInst(cmp);

		Ice::Variable *result = ::function->makeVariable(Ice::IceType_v4i32);
		auto select = Ice::InstSelect::create(::function, result, condition, y.value, x.value);
		::basicBlock->appendInst(select);

		return RValue<UInt4>(V(result));
	}

	RValue<UShort8> Pack(RValue<UInt4> x, RValue<UInt4> y)
	{
		Ice::Variable *result = ::function->makeVariable(Ice::IceType_v8i16);
		const Ice::Intrinsics::IntrinsicInfo intrinsic = {Ice::Intrinsics::VectorPackUnsigned, Ice::Intrinsics::SideEffects_F, Ice::Intrinsics::ReturnsTwice_F, Ice::Intrinsics::MemoryWrite_F};
		auto target = ::context->getConstantUndef(Ice::IceType_i32);
		auto pack = Ice::InstIntrinsicCall::create(::function, 2, result, target, intrinsic);
		pack->addArg(x.value);
		pack->addArg(y.value);
		::basicBlock->appendInst(pack);

		return RValue<UShort8>(V(result));
	}

	Type *UInt4::getType()
	{
		return T(Ice::IceType_v4i32);
	}

	Float::Float(RValue<Int> cast)
	{
		Value *integer = Nucleus::createSIToFP(cast.value, Float::getType());

		storeValue(integer);
	}

	Float::Float()
	{
	}

	Float::Float(float x)
	{
		storeValue(Nucleus::createConstantFloat(x));
	}

	Float::Float(RValue<Float> rhs)
	{
		storeValue(rhs.value);
	}

	Float::Float(const Float &rhs)
	{
		Value *value = rhs.loadValue();
		storeValue(value);
	}

	Float::Float(const Reference<Float> &rhs)
	{
		Value *value = rhs.loadValue();
		storeValue(value);
	}

	RValue<Float> Float::operator=(RValue<Float> rhs) const
	{
		storeValue(rhs.value);

		return rhs;
	}

	RValue<Float> Float::operator=(const Float &rhs) const
	{
		Value *value = rhs.loadValue();
		storeValue(value);

		return RValue<Float>(value);
	}

	RValue<Float> Float::operator=(const Reference<Float> &rhs) const
	{
		Value *value = rhs.loadValue();
		storeValue(value);

		return RValue<Float>(value);
	}

	RValue<Float> operator+(RValue<Float> lhs, RValue<Float> rhs)
	{
		return RValue<Float>(Nucleus::createFAdd(lhs.value, rhs.value));
	}

	RValue<Float> operator-(RValue<Float> lhs, RValue<Float> rhs)
	{
		return RValue<Float>(Nucleus::createFSub(lhs.value, rhs.value));
	}

	RValue<Float> operator*(RValue<Float> lhs, RValue<Float> rhs)
	{
		return RValue<Float>(Nucleus::createFMul(lhs.value, rhs.value));
	}

	RValue<Float> operator/(RValue<Float> lhs, RValue<Float> rhs)
	{
		return RValue<Float>(Nucleus::createFDiv(lhs.value, rhs.value));
	}

	RValue<Float> operator+=(const Float &lhs, RValue<Float> rhs)
	{
		return lhs = lhs + rhs;
	}

	RValue<Float> operator-=(const Float &lhs, RValue<Float> rhs)
	{
		return lhs = lhs - rhs;
	}

	RValue<Float> operator*=(const Float &lhs, RValue<Float> rhs)
	{
		return lhs = lhs * rhs;
	}

	RValue<Float> operator/=(const Float &lhs, RValue<Float> rhs)
	{
		return lhs = lhs / rhs;
	}

	RValue<Float> operator+(RValue<Float> val)
	{
		return val;
	}

	RValue<Float> operator-(RValue<Float> val)
	{
		return RValue<Float>(Nucleus::createFNeg(val.value));
	}

	RValue<Bool> operator<(RValue<Float> lhs, RValue<Float> rhs)
	{
		return RValue<Bool>(Nucleus::createFCmpOLT(lhs.value, rhs.value));
	}

	RValue<Bool> operator<=(RValue<Float> lhs, RValue<Float> rhs)
	{
		return RValue<Bool>(Nucleus::createFCmpOLE(lhs.value, rhs.value));
	}

	RValue<Bool> operator>(RValue<Float> lhs, RValue<Float> rhs)
	{
		return RValue<Bool>(Nucleus::createFCmpOGT(lhs.value, rhs.value));
	}

	RValue<Bool> operator>=(RValue<Float> lhs, RValue<Float> rhs)
	{
		return RValue<Bool>(Nucleus::createFCmpOGE(lhs.value, rhs.value));
	}

	RValue<Bool> operator!=(RValue<Float> lhs, RValue<Float> rhs)
	{
		return RValue<Bool>(Nucleus::createFCmpONE(lhs.value, rhs.value));
	}

	RValue<Bool> operator==(RValue<Float> lhs, RValue<Float> rhs)
	{
		return RValue<Bool>(Nucleus::createFCmpOEQ(lhs.value, rhs.value));
	}

	RValue<Float> Abs(RValue<Float> x)
	{
		return IfThenElse(x > 0.0f, x, -x);
	}

	RValue<Float> Max(RValue<Float> x, RValue<Float> y)
	{
		return IfThenElse(x > y, x, y);
	}

	RValue<Float> Min(RValue<Float> x, RValue<Float> y)
	{
		return IfThenElse(x < y, x, y);
	}

	RValue<Float> Rcp_pp(RValue<Float> x, bool exactAtPow2)
	{
		return 1.0f / x;
	}

	RValue<Float> RcpSqrt_pp(RValue<Float> x)
	{
		return Rcp_pp(Sqrt(x));
	}

	RValue<Float> Sqrt(RValue<Float> x)
	{
		Ice::Variable *result = ::function->makeVariable(Ice::IceType_f32);
		const Ice::Intrinsics::IntrinsicInfo intrinsic = {Ice::Intrinsics::Sqrt, Ice::Intrinsics::SideEffects_F, Ice::Intrinsics::ReturnsTwice_F, Ice::Intrinsics::MemoryWrite_F};
		auto target = ::context->getConstantUndef(Ice::IceType_i32);
		auto sqrt = Ice::InstIntrinsicCall::create(::function, 1, result, target, intrinsic);
		sqrt->addArg(x.value);
		::basicBlock->appendInst(sqrt);

		return RValue<Float>(V(result));
	}

	RValue<Float> Round(RValue<Float> x)
	{
		assert(false && "UNIMPLEMENTED"); return RValue<Float>(V(nullptr));
	}

	RValue<Float> Trunc(RValue<Float> x)
	{
		assert(false && "UNIMPLEMENTED"); return RValue<Float>(V(nullptr));
	}

	RValue<Float> Frac(RValue<Float> x)
	{
		assert(false && "UNIMPLEMENTED"); return RValue<Float>(V(nullptr));
	}

	RValue<Float> Floor(RValue<Float> x)
	{
		assert(false && "UNIMPLEMENTED"); return RValue<Float>(V(nullptr));
	}

	RValue<Float> Ceil(RValue<Float> x)
	{
		assert(false && "UNIMPLEMENTED"); return RValue<Float>(V(nullptr));
	}

	Type *Float::getType()
	{
		return T(Ice::IceType_f32);
	}

	Float2::Float2(RValue<Float4> cast)
	{
		storeValue(Nucleus::createBitCast(cast.value, getType()));
	}

	Type *Float2::getType()
	{
		return T(Type_v2f32);
	}

	Float4::Float4(RValue<Byte4> cast)
	{
		xyzw.parent = this;

		assert(false && "UNIMPLEMENTED");
	}

	Float4::Float4(RValue<SByte4> cast)
	{
		xyzw.parent = this;

		assert(false && "UNIMPLEMENTED");
	}

	Float4::Float4(RValue<Short4> cast)
	{
		xyzw.parent = this;

		Int4 c(cast);
		storeValue(Nucleus::createSIToFP(RValue<Int4>(c).value, Float4::getType()));
	}

	Float4::Float4(RValue<UShort4> cast)
	{
		xyzw.parent = this;

		Int4 c(cast);
		storeValue(Nucleus::createSIToFP(RValue<Int4>(c).value, Float4::getType()));
	}

	Float4::Float4(RValue<Int4> cast)
	{
		xyzw.parent = this;

		Value *xyzw = Nucleus::createSIToFP(cast.value, Float4::getType());

		storeValue(xyzw);
	}

	Float4::Float4(RValue<UInt4> cast)
	{
		xyzw.parent = this;

		Value *xyzw = Nucleus::createUIToFP(cast.value, Float4::getType());

		storeValue(xyzw);
	}

	Float4::Float4()
	{
		xyzw.parent = this;
	}

	Float4::Float4(float xyzw)
	{
		constant(xyzw, xyzw, xyzw, xyzw);
	}

	Float4::Float4(float x, float yzw)
	{
		constant(x, yzw, yzw, yzw);
	}

	Float4::Float4(float x, float y, float zw)
	{
		constant(x, y, zw, zw);
	}

	Float4::Float4(float x, float y, float z, float w)
	{
		constant(x, y, z, w);
	}

	void Float4::constant(float x, float y, float z, float w)
	{
		xyzw.parent = this;

		double constantVector[4] = {x, y, z, w};
		storeValue(Nucleus::createConstantVector(constantVector, getType()));
	}

	Float4::Float4(RValue<Float4> rhs)
	{
		xyzw.parent = this;

		storeValue(rhs.value);
	}

	Float4::Float4(const Float4 &rhs)
	{
		xyzw.parent = this;

		Value *value = rhs.loadValue();
		storeValue(value);
	}

	Float4::Float4(const Reference<Float4> &rhs)
	{
		xyzw.parent = this;

		Value *value = rhs.loadValue();
		storeValue(value);
	}

	Float4::Float4(RValue<Float> rhs)
	{
		xyzw.parent = this;

		assert(false && "UNIMPLEMENTED");
	}

	Float4::Float4(const Float &rhs)
	{
		xyzw.parent = this;

		*this = RValue<Float>(rhs.loadValue());
	}

	Float4::Float4(const Reference<Float> &rhs)
	{
		xyzw.parent = this;

		*this = RValue<Float>(rhs.loadValue());
	}

	RValue<Float4> Float4::operator=(float x) const
	{
		return *this = Float4(x, x, x, x);
	}

	RValue<Float4> Float4::operator=(RValue<Float4> rhs) const
	{
		storeValue(rhs.value);

		return rhs;
	}

	RValue<Float4> Float4::operator=(const Float4 &rhs) const
	{
		Value *value = rhs.loadValue();
		storeValue(value);

		return RValue<Float4>(value);
	}

	RValue<Float4> Float4::operator=(const Reference<Float4> &rhs) const
	{
		Value *value = rhs.loadValue();
		storeValue(value);

		return RValue<Float4>(value);
	}

	RValue<Float4> Float4::operator=(RValue<Float> rhs) const
	{
		return *this = Float4(rhs);
	}

	RValue<Float4> Float4::operator=(const Float &rhs) const
	{
		return *this = Float4(rhs);
	}

	RValue<Float4> Float4::operator=(const Reference<Float> &rhs) const
	{
		return *this = Float4(rhs);
	}

	RValue<Float4> operator+(RValue<Float4> lhs, RValue<Float4> rhs)
	{
		return RValue<Float4>(Nucleus::createFAdd(lhs.value, rhs.value));
	}

	RValue<Float4> operator-(RValue<Float4> lhs, RValue<Float4> rhs)
	{
		return RValue<Float4>(Nucleus::createFSub(lhs.value, rhs.value));
	}

	RValue<Float4> operator*(RValue<Float4> lhs, RValue<Float4> rhs)
	{
		return RValue<Float4>(Nucleus::createFMul(lhs.value, rhs.value));
	}

	RValue<Float4> operator/(RValue<Float4> lhs, RValue<Float4> rhs)
	{
		return RValue<Float4>(Nucleus::createFDiv(lhs.value, rhs.value));
	}

	RValue<Float4> operator%(RValue<Float4> lhs, RValue<Float4> rhs)
	{
		return RValue<Float4>(Nucleus::createFRem(lhs.value, rhs.value));
	}

	RValue<Float4> operator+=(const Float4 &lhs, RValue<Float4> rhs)
	{
		return lhs = lhs + rhs;
	}

	RValue<Float4> operator-=(const Float4 &lhs, RValue<Float4> rhs)
	{
		return lhs = lhs - rhs;
	}

	RValue<Float4> operator*=(const Float4 &lhs, RValue<Float4> rhs)
	{
		return lhs = lhs * rhs;
	}

	RValue<Float4> operator/=(const Float4 &lhs, RValue<Float4> rhs)
	{
		return lhs = lhs / rhs;
	}

	RValue<Float4> operator%=(const Float4 &lhs, RValue<Float4> rhs)
	{
		return lhs = lhs % rhs;
	}

	RValue<Float4> operator+(RValue<Float4> val)
	{
		return val;
	}

	RValue<Float4> operator-(RValue<Float4> val)
	{
		return RValue<Float4>(Nucleus::createFNeg(val.value));
	}

	RValue<Float4> Abs(RValue<Float4> x)
	{
		assert(false && "UNIMPLEMENTED"); return RValue<Float4>(V(nullptr));
	}

	RValue<Float4> Max(RValue<Float4> x, RValue<Float4> y)
	{
		Ice::Variable *condition = ::function->makeVariable(Ice::IceType_v4i1);
		auto cmp = Ice::InstFcmp::create(::function, Ice::InstFcmp::Ule, condition, x.value, y.value);
		::basicBlock->appendInst(cmp);

		Ice::Variable *result = ::function->makeVariable(Ice::IceType_v4f32);
		auto select = Ice::InstSelect::create(::function, result, condition, y.value, x.value);
		::basicBlock->appendInst(select);

		return RValue<Float4>(V(result));
	}

	RValue<Float4> Min(RValue<Float4> x, RValue<Float4> y)
	{
		Ice::Variable *condition = ::function->makeVariable(Ice::IceType_v4i1);
		auto cmp = Ice::InstFcmp::create(::function, Ice::InstFcmp::Ugt, condition, x.value, y.value);
		::basicBlock->appendInst(cmp);

		Ice::Variable *result = ::function->makeVariable(Ice::IceType_v4f32);
		auto select = Ice::InstSelect::create(::function, result, condition, y.value, x.value);
		::basicBlock->appendInst(select);

		return RValue<Float4>(V(result));
	}

	RValue<Float4> Rcp_pp(RValue<Float4> x, bool exactAtPow2)
	{
		return Float4(1.0f) / x;
	}

	RValue<Float4> RcpSqrt_pp(RValue<Float4> x)
	{
		return Rcp_pp(Sqrt(x));
	}

	RValue<Float4> Sqrt(RValue<Float4> x)
	{
		Ice::Variable *result = ::function->makeVariable(Ice::IceType_v4f32);
		const Ice::Intrinsics::IntrinsicInfo intrinsic = {Ice::Intrinsics::Sqrt, Ice::Intrinsics::SideEffects_F, Ice::Intrinsics::ReturnsTwice_F, Ice::Intrinsics::MemoryWrite_F};
		auto target = ::context->getConstantUndef(Ice::IceType_i32);
		auto sqrt = Ice::InstIntrinsicCall::create(::function, 1, result, target, intrinsic);
		sqrt->addArg(x.value);
		::basicBlock->appendInst(sqrt);

		return RValue<Float4>(V(result));
	}

	RValue<Float4> Insert(const Float4 &val, RValue<Float> element, int i)
	{
		Value *value = val.loadValue();
		Value *insert = Nucleus::createInsertElement(value, element.value, i);

		val = RValue<Float4>(insert);

		return val;
	}

	RValue<Float> Extract(RValue<Float4> x, int i)
	{
		return RValue<Float>(Nucleus::createExtractElement(x.value, Float::getType(), i));
	}

	RValue<Float4> Swizzle(RValue<Float4> x, unsigned char select)
	{
		return RValue<Float4>(createSwizzle4(x.value, select));
	}

	RValue<Float4> ShuffleLowHigh(RValue<Float4> x, RValue<Float4> y, unsigned char imm)
	{
		int shuffle[4] =
		{
			((imm >> 0) & 0x03) + 0,
			((imm >> 2) & 0x03) + 0,
			((imm >> 4) & 0x03) + 4,
			((imm >> 6) & 0x03) + 4,
		};

		return RValue<Float4>(Nucleus::createShuffleVector(x.value, y.value, shuffle));
	}

	RValue<Float4> UnpackLow(RValue<Float4> x, RValue<Float4> y)
	{
		int shuffle[4] = {0, 4, 1, 5};
		return RValue<Float4>(Nucleus::createShuffleVector(x.value, y.value, shuffle));
	}

	RValue<Float4> UnpackHigh(RValue<Float4> x, RValue<Float4> y)
	{
		int shuffle[4] = {2, 6, 3, 7};
		return RValue<Float4>(Nucleus::createShuffleVector(x.value, y.value, shuffle));
	}

	RValue<Float4> Mask(Float4 &lhs, RValue<Float4> rhs, unsigned char select)
	{
		Value *vector = lhs.loadValue();
		Value *shuffle = createMask4(vector, rhs.value, select);
		lhs.storeValue(shuffle);

		return RValue<Float4>(shuffle);
	}

	RValue<Int> SignMask(RValue<Float4> x)
	{
		Ice::Variable *result = ::function->makeVariable(Ice::IceType_i32);
		const Ice::Intrinsics::IntrinsicInfo intrinsic = {Ice::Intrinsics::SignMask, Ice::Intrinsics::SideEffects_F, Ice::Intrinsics::ReturnsTwice_F, Ice::Intrinsics::MemoryWrite_F};
		auto target = ::context->getConstantUndef(Ice::IceType_i32);
		auto movmsk = Ice::InstIntrinsicCall::create(::function, 1, result, target, intrinsic);
		movmsk->addArg(x.value);
		::basicBlock->appendInst(movmsk);

		return RValue<Int>(V(result));
	}

	RValue<Int4> CmpEQ(RValue<Float4> x, RValue<Float4> y)
	{
		return RValue<Int4>(Nucleus::createSExt(Nucleus::createFCmpOEQ(x.value, y.value), Int4::getType()));
	}

	RValue<Int4> CmpLT(RValue<Float4> x, RValue<Float4> y)
	{
		return RValue<Int4>(Nucleus::createSExt(Nucleus::createFCmpOLT(x.value, y.value), Int4::getType()));
	}

	RValue<Int4> CmpLE(RValue<Float4> x, RValue<Float4> y)
	{
		return RValue<Int4>(Nucleus::createSExt(Nucleus::createFCmpOLE(x.value, y.value), Int4::getType()));
	}

	RValue<Int4> CmpNEQ(RValue<Float4> x, RValue<Float4> y)
	{
		return RValue<Int4>(Nucleus::createSExt(Nucleus::createFCmpONE(x.value, y.value), Int4::getType()));
	}

	RValue<Int4> CmpNLT(RValue<Float4> x, RValue<Float4> y)
	{
		return RValue<Int4>(Nucleus::createSExt(Nucleus::createFCmpOGE(x.value, y.value), Int4::getType()));
	}

	RValue<Int4> CmpNLE(RValue<Float4> x, RValue<Float4> y)
	{
		return RValue<Int4>(Nucleus::createSExt(Nucleus::createFCmpOGT(x.value, y.value), Int4::getType()));
	}

	RValue<Float4> Round(RValue<Float4> x)
	{
		assert(false && "UNIMPLEMENTED"); return RValue<Float4>(V(nullptr));
	}

	RValue<Float4> Trunc(RValue<Float4> x)
	{
		assert(false && "UNIMPLEMENTED"); return RValue<Float4>(V(nullptr));
	}

	RValue<Float4> Frac(RValue<Float4> x)
	{
		assert(false && "UNIMPLEMENTED"); return RValue<Float4>(V(nullptr));
	}

	RValue<Float4> Floor(RValue<Float4> x)
	{
		assert(false && "UNIMPLEMENTED"); return RValue<Float4>(V(nullptr));
	}

	RValue<Float4> Ceil(RValue<Float4> x)
	{
		assert(false && "UNIMPLEMENTED"); return RValue<Float4>(V(nullptr));
	}

	Type *Float4::getType()
	{
		return T(Ice::IceType_v4f32);
	}

	RValue<Pointer<Byte>> operator+(RValue<Pointer<Byte>> lhs, int offset)
	{
		return lhs + RValue<Int>(Nucleus::createConstantInt(offset));
	}

	RValue<Pointer<Byte>> operator+(RValue<Pointer<Byte>> lhs, RValue<Int> offset)
	{
		return RValue<Pointer<Byte>>(Nucleus::createGEP(lhs.value, Byte::getType(), offset.value));
	}

	RValue<Pointer<Byte>> operator+(RValue<Pointer<Byte>> lhs, RValue<UInt> offset)
	{
		return RValue<Pointer<Byte>>(Nucleus::createGEP(lhs.value, Byte::getType(), offset.value));
	}

	RValue<Pointer<Byte>> operator+=(const Pointer<Byte> &lhs, int offset)
	{
		return lhs = lhs + offset;
	}

	RValue<Pointer<Byte>> operator+=(const Pointer<Byte> &lhs, RValue<Int> offset)
	{
		return lhs = lhs + offset;
	}

	RValue<Pointer<Byte>> operator+=(const Pointer<Byte> &lhs, RValue<UInt> offset)
	{
		return lhs = lhs + offset;
	}

	RValue<Pointer<Byte>> operator-(RValue<Pointer<Byte>> lhs, int offset)
	{
		return lhs + -offset;
	}

	RValue<Pointer<Byte>> operator-(RValue<Pointer<Byte>> lhs, RValue<Int> offset)
	{
		return lhs + -offset;
	}

	RValue<Pointer<Byte>> operator-(RValue<Pointer<Byte>> lhs, RValue<UInt> offset)
	{
		return lhs + -offset;
	}

	RValue<Pointer<Byte>> operator-=(const Pointer<Byte> &lhs, int offset)
	{
		return lhs = lhs - offset;
	}

	RValue<Pointer<Byte>> operator-=(const Pointer<Byte> &lhs, RValue<Int> offset)
	{
		return lhs = lhs - offset;
	}

	RValue<Pointer<Byte>> operator-=(const Pointer<Byte> &lhs, RValue<UInt> offset)
	{
		return lhs = lhs - offset;
	}

	void Return()
	{
		Nucleus::createRetVoid();
		Nucleus::setInsertBlock(Nucleus::createBasicBlock());
		Nucleus::createUnreachable();
	}

	void Return(bool ret)
	{
		Nucleus::createRet(Nucleus::createConstantInt(ret));
		Nucleus::setInsertBlock(Nucleus::createBasicBlock());
		Nucleus::createUnreachable();
	}

	void Return(const Int &ret)
	{
		Nucleus::createRet(ret.loadValue());
		Nucleus::setInsertBlock(Nucleus::createBasicBlock());
		Nucleus::createUnreachable();
	}

	bool branch(RValue<Bool> cmp, BasicBlock *bodyBB, BasicBlock *endBB)
	{
		Nucleus::createCondBr(cmp.value, bodyBB, endBB);
		Nucleus::setInsertBlock(bodyBB);

		return true;
	}

	void endIf(BasicBlock *falseBB)
	{
		::falseBB = falseBB;
	}

	bool elseBlock(BasicBlock *falseBB)
	{
		assert(falseBB && "Else not preceded by If");
		falseBB->getInsts().back().setDeleted();
		Nucleus::setInsertBlock(falseBB);

		return true;
	}

	BasicBlock *beginElse()
	{
		BasicBlock *falseBB = ::falseBB;
		::falseBB = nullptr;

		return falseBB;
	}

	RValue<Long> Ticks()
	{
		assert(false && "UNIMPLEMENTED"); return RValue<Long>(V(nullptr));
	}
}
