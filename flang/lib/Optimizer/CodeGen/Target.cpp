//===-- Target.cpp --------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Coding style: https://mlir.llvm.org/getting_started/DeveloperGuide/
//
//===----------------------------------------------------------------------===//

#include "Target.h"
#include "flang/Optimizer/Builder/Todo.h"
#include "flang/Optimizer/Dialect/FIRType.h"
#include "flang/Optimizer/Support/FatalError.h"
#include "flang/Optimizer/Support/KindMapping.h"
#include "mlir/IR/BuiltinTypes.h"
#include "mlir/IR/TypeRange.h"

#define DEBUG_TYPE "flang-codegen-target"

using namespace fir;

// Reduce a REAL/float type to the floating point semantics.
static const llvm::fltSemantics &floatToSemantics(const KindMapping &kindMap,
                                                  mlir::Type type) {
  assert(isa_real(type));
  if (auto ty = type.dyn_cast<fir::RealType>())
    return kindMap.getFloatSemantics(ty.getFKind());
  return type.cast<mlir::FloatType>().getFloatSemantics();
}

namespace {
template <typename S>
struct GenericTarget : public CodeGenSpecifics {
  using CodeGenSpecifics::CodeGenSpecifics;
  using AT = CodeGenSpecifics::Attributes;

  mlir::Type complexMemoryType(mlir::Type eleTy) const override {
    assert(fir::isa_real(eleTy));
    // Use a type that will be translated into LLVM as:
    // { t, t }   struct of 2 eleTy
    mlir::TypeRange range = {eleTy, eleTy};
    return mlir::TupleType::get(eleTy.getContext(), range);
  }

  mlir::Type boxcharMemoryType(mlir::Type eleTy) const override {
    auto idxTy = mlir::IntegerType::get(eleTy.getContext(), S::defaultWidth);
    auto ptrTy = fir::ReferenceType::get(eleTy);
    // Use a type that will be translated into LLVM as:
    // { t*, index }
    mlir::TypeRange range = {ptrTy, idxTy};
    return mlir::TupleType::get(eleTy.getContext(), range);
  }

  Marshalling boxcharArgumentType(mlir::Type eleTy, bool sret) const override {
    CodeGenSpecifics::Marshalling marshal;
    auto idxTy = mlir::IntegerType::get(eleTy.getContext(), S::defaultWidth);
    auto ptrTy = fir::ReferenceType::get(eleTy);
    marshal.emplace_back(ptrTy, AT{});
    // Return value arguments are grouped as a pair. Others are passed in a
    // split format with all pointers first (in the declared position) and all
    // LEN arguments appended after all of the dummy arguments.
    // NB: Other conventions/ABIs can/should be supported via options.
    marshal.emplace_back(idxTy, AT{/*alignment=*/0, /*byval=*/false,
                                   /*sret=*/sret, /*append=*/!sret});
    return marshal;
  }
};
} // namespace

//===----------------------------------------------------------------------===//
// i386 (x86 32 bit) linux target specifics.
//===----------------------------------------------------------------------===//

namespace {
struct TargetI386 : public GenericTarget<TargetI386> {
  using GenericTarget::GenericTarget;

  static constexpr int defaultWidth = 32;

  CodeGenSpecifics::Marshalling
  complexArgumentType(mlir::Location, mlir::Type eleTy) const override {
    assert(fir::isa_real(eleTy));
    CodeGenSpecifics::Marshalling marshal;
    // Use a type that will be translated into LLVM as:
    // { t, t }   struct of 2 eleTy, byval, align 4
    mlir::TypeRange range = {eleTy, eleTy};
    auto structTy = mlir::TupleType::get(eleTy.getContext(), range);
    marshal.emplace_back(fir::ReferenceType::get(structTy),
                         AT{/*alignment=*/4, /*byval=*/true});
    return marshal;
  }

  CodeGenSpecifics::Marshalling
  complexReturnType(mlir::Location loc, mlir::Type eleTy) const override {
    assert(fir::isa_real(eleTy));
    CodeGenSpecifics::Marshalling marshal;
    const auto *sem = &floatToSemantics(kindMap, eleTy);
    if (sem == &llvm::APFloat::IEEEsingle()) {
      // i64   pack both floats in a 64-bit GPR
      marshal.emplace_back(mlir::IntegerType::get(eleTy.getContext(), 64),
                           AT{});
    } else if (sem == &llvm::APFloat::IEEEdouble()) {
      // Use a type that will be translated into LLVM as:
      // { t, t }   struct of 2 eleTy, sret, align 4
      mlir::TypeRange range = {eleTy, eleTy};
      auto structTy = mlir::TupleType::get(eleTy.getContext(), range);
      marshal.emplace_back(fir::ReferenceType::get(structTy),
                           AT{/*alignment=*/4, /*byval=*/false, /*sret=*/true});
    } else {
      TODO(loc, "complex for this precision");
    }
    return marshal;
  }
};
} // namespace

//===----------------------------------------------------------------------===//
// i386 (x86 32 bit) Windows target specifics.
//===----------------------------------------------------------------------===//

namespace {
struct TargetI386Win : public GenericTarget<TargetI386Win> {
  using GenericTarget::GenericTarget;

  static constexpr int defaultWidth = 32;

  CodeGenSpecifics::Marshalling
  complexArgumentType(mlir::Location loc, mlir::Type eleTy) const override {
    CodeGenSpecifics::Marshalling marshal;
    // Use a type that will be translated into LLVM as:
    // { t, t }   struct of 2 eleTy, byval, align 4
    auto structTy =
        mlir::TupleType::get(eleTy.getContext(), mlir::TypeRange{eleTy, eleTy});
    marshal.emplace_back(fir::ReferenceType::get(structTy),
                         AT{/*align=*/4, /*byval=*/true});
    return marshal;
  }

  CodeGenSpecifics::Marshalling
  complexReturnType(mlir::Location loc, mlir::Type eleTy) const override {
    CodeGenSpecifics::Marshalling marshal;
    const auto *sem = &floatToSemantics(kindMap, eleTy);
    if (sem == &llvm::APFloat::IEEEsingle()) {
      // i64   pack both floats in a 64-bit GPR
      marshal.emplace_back(mlir::IntegerType::get(eleTy.getContext(), 64),
                           AT{});
    } else if (sem == &llvm::APFloat::IEEEdouble()) {
      // Use a type that will be translated into LLVM as:
      // { double, double }   struct of 2 double, sret, align 8
      marshal.emplace_back(
          fir::ReferenceType::get(mlir::TupleType::get(
              eleTy.getContext(), mlir::TypeRange{eleTy, eleTy})),
          AT{/*align=*/8, /*byval=*/false, /*sret=*/true});
    } else if (sem == &llvm::APFloat::IEEEquad()) {
      // Use a type that will be translated into LLVM as:
      // { fp128, fp128 }   struct of 2 fp128, sret, align 16
      marshal.emplace_back(
          fir::ReferenceType::get(mlir::TupleType::get(
              eleTy.getContext(), mlir::TypeRange{eleTy, eleTy})),
          AT{/*align=*/16, /*byval=*/false, /*sret=*/true});
    } else if (sem == &llvm::APFloat::x87DoubleExtended()) {
      // Use a type that will be translated into LLVM as:
      // { x86_fp80, x86_fp80 }   struct of 2 x86_fp80, sret, align 4
      marshal.emplace_back(
          fir::ReferenceType::get(mlir::TupleType::get(
              eleTy.getContext(), mlir::TypeRange{eleTy, eleTy})),
          AT{/*align=*/4, /*byval=*/false, /*sret=*/true});
    } else {
      TODO(loc, "complex for this precision");
    }
    return marshal;
  }
};
} // namespace

//===----------------------------------------------------------------------===//
// x86_64 (x86 64 bit) linux target specifics.
//===----------------------------------------------------------------------===//

namespace {
struct TargetX86_64 : public GenericTarget<TargetX86_64> {
  using GenericTarget::GenericTarget;

  static constexpr int defaultWidth = 64;

  CodeGenSpecifics::Marshalling
  complexArgumentType(mlir::Location loc, mlir::Type eleTy) const override {
    CodeGenSpecifics::Marshalling marshal;
    const auto *sem = &floatToSemantics(kindMap, eleTy);
    if (sem == &llvm::APFloat::IEEEsingle()) {
      // <2 x t>   vector of 2 eleTy
      marshal.emplace_back(fir::VectorType::get(2, eleTy), AT{});
    } else if (sem == &llvm::APFloat::IEEEdouble()) {
      // two distinct double arguments
      marshal.emplace_back(eleTy, AT{});
      marshal.emplace_back(eleTy, AT{});
    } else if (sem == &llvm::APFloat::IEEEquad()) {
      // Use a type that will be translated into LLVM as:
      // { fp128, fp128 }   struct of 2 fp128, byval, align 16
      mlir::TypeRange range = {eleTy, eleTy};
      marshal.emplace_back(fir::ReferenceType::get(
                               mlir::TupleType::get(eleTy.getContext(), range)),
                           AT{/*align=*/16, /*byval=*/true});
    } else {
      TODO(loc, "complex for this precision");
    }
    return marshal;
  }

  CodeGenSpecifics::Marshalling
  complexReturnType(mlir::Location loc, mlir::Type eleTy) const override {
    CodeGenSpecifics::Marshalling marshal;
    const auto *sem = &floatToSemantics(kindMap, eleTy);
    if (sem == &llvm::APFloat::IEEEsingle()) {
      // <2 x t>   vector of 2 eleTy
      marshal.emplace_back(fir::VectorType::get(2, eleTy), AT{});
    } else if (sem == &llvm::APFloat::IEEEdouble()) {
      // Use a type that will be translated into LLVM as:
      // { double, double }   struct of 2 double
      mlir::TypeRange range = {eleTy, eleTy};
      marshal.emplace_back(mlir::TupleType::get(eleTy.getContext(), range),
                           AT{});
    } else if (sem == &llvm::APFloat::IEEEquad()) {
      // Use a type that will be translated into LLVM as:
      // { fp128, fp128 }   struct of 2 fp128, sret, align 16
      mlir::TypeRange range = {eleTy, eleTy};
      marshal.emplace_back(fir::ReferenceType::get(
                               mlir::TupleType::get(eleTy.getContext(), range)),
                           AT{/*align=*/16, /*byval=*/false, /*sret=*/true});
    } else {
      TODO(loc, "complex for this precision");
    }
    return marshal;
  }
};
} // namespace

//===----------------------------------------------------------------------===//
// x86_64 (x86 64 bit) Windows target specifics.
//===----------------------------------------------------------------------===//

namespace {
struct TargetX86_64Win : public GenericTarget<TargetX86_64Win> {
  using GenericTarget::GenericTarget;

  static constexpr int defaultWidth = 64;

  CodeGenSpecifics::Marshalling
  complexArgumentType(mlir::Location loc, mlir::Type eleTy) const override {
    CodeGenSpecifics::Marshalling marshal;
    const auto *sem = &floatToSemantics(kindMap, eleTy);
    if (sem == &llvm::APFloat::IEEEsingle()) {
      // i64   pack both floats in a 64-bit GPR
      marshal.emplace_back(mlir::IntegerType::get(eleTy.getContext(), 64),
                           AT{});
    } else if (sem == &llvm::APFloat::IEEEdouble()) {
      // Use a type that will be translated into LLVM as:
      // { double, double }   struct of 2 double, byval, align 8
      marshal.emplace_back(
          fir::ReferenceType::get(mlir::TupleType::get(
              eleTy.getContext(), mlir::TypeRange{eleTy, eleTy})),
          AT{/*align=*/8, /*byval=*/true});
    } else if (sem == &llvm::APFloat::IEEEquad() ||
               sem == &llvm::APFloat::x87DoubleExtended()) {
      // Use a type that will be translated into LLVM as:
      // { t, t }   struct of 2 eleTy, byval, align 16
      marshal.emplace_back(
          fir::ReferenceType::get(mlir::TupleType::get(
              eleTy.getContext(), mlir::TypeRange{eleTy, eleTy})),
          AT{/*align=*/16, /*byval=*/true});
    } else {
      TODO(loc, "complex for this precision");
    }
    return marshal;
  }

  CodeGenSpecifics::Marshalling
  complexReturnType(mlir::Location loc, mlir::Type eleTy) const override {
    CodeGenSpecifics::Marshalling marshal;
    const auto *sem = &floatToSemantics(kindMap, eleTy);
    if (sem == &llvm::APFloat::IEEEsingle()) {
      // i64   pack both floats in a 64-bit GPR
      marshal.emplace_back(mlir::IntegerType::get(eleTy.getContext(), 64),
                           AT{});
    } else if (sem == &llvm::APFloat::IEEEdouble()) {
      // Use a type that will be translated into LLVM as:
      // { double, double }   struct of 2 double, sret, align 8
      marshal.emplace_back(
          fir::ReferenceType::get(mlir::TupleType::get(
              eleTy.getContext(), mlir::TypeRange{eleTy, eleTy})),
          AT{/*align=*/8, /*byval=*/false, /*sret=*/true});
    } else if (sem == &llvm::APFloat::IEEEquad() ||
               sem == &llvm::APFloat::x87DoubleExtended()) {
      // Use a type that will be translated into LLVM as:
      // { t, t }   struct of 2 eleTy, sret, align 16
      marshal.emplace_back(
          fir::ReferenceType::get(mlir::TupleType::get(
              eleTy.getContext(), mlir::TypeRange{eleTy, eleTy})),
          AT{/*align=*/16, /*byval=*/false, /*sret=*/true});
    } else {
      TODO(loc, "complex for this precision");
    }
    return marshal;
  }
};
} // namespace

//===----------------------------------------------------------------------===//
// AArch64 linux target specifics.
//===----------------------------------------------------------------------===//

namespace {
struct TargetAArch64 : public GenericTarget<TargetAArch64> {
  using GenericTarget::GenericTarget;

  static constexpr int defaultWidth = 64;

  CodeGenSpecifics::Marshalling
  complexArgumentType(mlir::Location loc, mlir::Type eleTy) const override {
    CodeGenSpecifics::Marshalling marshal;
    const auto *sem = &floatToSemantics(kindMap, eleTy);
    if (sem == &llvm::APFloat::IEEEsingle() ||
        sem == &llvm::APFloat::IEEEdouble()) {
      // [2 x t]   array of 2 eleTy
      marshal.emplace_back(fir::SequenceType::get({2}, eleTy), AT{});
    } else {
      TODO(loc, "complex for this precision");
    }
    return marshal;
  }

  CodeGenSpecifics::Marshalling
  complexReturnType(mlir::Location loc, mlir::Type eleTy) const override {
    CodeGenSpecifics::Marshalling marshal;
    const auto *sem = &floatToSemantics(kindMap, eleTy);
    if (sem == &llvm::APFloat::IEEEsingle() ||
        sem == &llvm::APFloat::IEEEdouble()) {
      // Use a type that will be translated into LLVM as:
      // { t, t }   struct of 2 eleTy
      mlir::TypeRange range = {eleTy, eleTy};
      marshal.emplace_back(mlir::TupleType::get(eleTy.getContext(), range),
                           AT{});
    } else {
      TODO(loc, "complex for this precision");
    }
    return marshal;
  }
};
} // namespace

//===----------------------------------------------------------------------===//
// PPC64 (AIX 64 bit) target specifics.
//===----------------------------------------------------------------------===//

namespace {
struct TargetPPC64 : public GenericTarget<TargetPPC64> {
  using GenericTarget::GenericTarget;

  static constexpr int defaultWidth = 64;

  CodeGenSpecifics::Marshalling
  complexArgumentType(mlir::Location, mlir::Type eleTy) const override {
    CodeGenSpecifics::Marshalling marshal;
    // two distinct element type arguments (re, im)
    marshal.emplace_back(eleTy, AT{});
    marshal.emplace_back(eleTy, AT{});
    return marshal;
  }

  CodeGenSpecifics::Marshalling
  complexReturnType(mlir::Location, mlir::Type eleTy) const override {
    CodeGenSpecifics::Marshalling marshal;
    // Use a type that will be translated into LLVM as:
    // { t, t }   struct of 2 element type
    mlir::TypeRange range = {eleTy, eleTy};
    marshal.emplace_back(mlir::TupleType::get(eleTy.getContext(), range), AT{});
    return marshal;
  }
};
} // namespace

//===----------------------------------------------------------------------===//
// PPC64le linux target specifics.
//===----------------------------------------------------------------------===//

namespace {
struct TargetPPC64le : public GenericTarget<TargetPPC64le> {
  using GenericTarget::GenericTarget;

  static constexpr int defaultWidth = 64;

  CodeGenSpecifics::Marshalling
  complexArgumentType(mlir::Location, mlir::Type eleTy) const override {
    CodeGenSpecifics::Marshalling marshal;
    // two distinct element type arguments (re, im)
    marshal.emplace_back(eleTy, AT{});
    marshal.emplace_back(eleTy, AT{});
    return marshal;
  }

  CodeGenSpecifics::Marshalling
  complexReturnType(mlir::Location, mlir::Type eleTy) const override {
    CodeGenSpecifics::Marshalling marshal;
    // Use a type that will be translated into LLVM as:
    // { t, t }   struct of 2 element type
    mlir::TypeRange range = {eleTy, eleTy};
    marshal.emplace_back(mlir::TupleType::get(eleTy.getContext(), range), AT{});
    return marshal;
  }
};
} // namespace

//===----------------------------------------------------------------------===//
// sparc (sparc 32 bit) target specifics.
//===----------------------------------------------------------------------===//

namespace {
struct TargetSparc : public GenericTarget<TargetSparc> {
  using GenericTarget::GenericTarget;

  static constexpr int defaultWidth = 32;

  CodeGenSpecifics::Marshalling
  complexArgumentType(mlir::Location, mlir::Type eleTy) const override {
    assert(fir::isa_real(eleTy));
    CodeGenSpecifics::Marshalling marshal;
    // Use a type that will be translated into LLVM as:
    // { t, t }   struct of 2 eleTy
    mlir::TypeRange range = {eleTy, eleTy};
    auto structTy = mlir::TupleType::get(eleTy.getContext(), range);
    marshal.emplace_back(fir::ReferenceType::get(structTy), AT{});
    return marshal;
  }

  CodeGenSpecifics::Marshalling
  complexReturnType(mlir::Location loc, mlir::Type eleTy) const override {
    assert(fir::isa_real(eleTy));
    CodeGenSpecifics::Marshalling marshal;
    // Use a type that will be translated into LLVM as:
    // { t, t }   struct of 2 eleTy, byval
    mlir::TypeRange range = {eleTy, eleTy};
    auto structTy = mlir::TupleType::get(eleTy.getContext(), range);
    marshal.emplace_back(fir::ReferenceType::get(structTy),
                         AT{/*alignment=*/0, /*byval=*/true});
    return marshal;
  }
};
} // namespace

//===----------------------------------------------------------------------===//
// sparcv9 (sparc 64 bit) target specifics.
//===----------------------------------------------------------------------===//

namespace {
struct TargetSparcV9 : public GenericTarget<TargetSparcV9> {
  using GenericTarget::GenericTarget;

  static constexpr int defaultWidth = 64;

  CodeGenSpecifics::Marshalling
  complexArgumentType(mlir::Location loc, mlir::Type eleTy) const override {
    CodeGenSpecifics::Marshalling marshal;
    const auto *sem = &floatToSemantics(kindMap, eleTy);
    if (sem == &llvm::APFloat::IEEEsingle() ||
        sem == &llvm::APFloat::IEEEdouble()) {
      // two distinct float, double arguments
      marshal.emplace_back(eleTy, AT{});
      marshal.emplace_back(eleTy, AT{});
    } else if (sem == &llvm::APFloat::IEEEquad()) {
      // Use a type that will be translated into LLVM as:
      // { fp128, fp128 }   struct of 2 fp128, byval, align 16
      mlir::TypeRange range = {eleTy, eleTy};
      marshal.emplace_back(fir::ReferenceType::get(
                               mlir::TupleType::get(eleTy.getContext(), range)),
                           AT{/*align=*/16, /*byval=*/true});
    } else {
      TODO(loc, "complex for this precision");
    }
    return marshal;
  }

  CodeGenSpecifics::Marshalling
  complexReturnType(mlir::Location loc, mlir::Type eleTy) const override {
    CodeGenSpecifics::Marshalling marshal;
    // Use a type that will be translated into LLVM as:
    // { eleTy, eleTy }   struct of 2 eleTy
    mlir::TypeRange range = {eleTy, eleTy};
    marshal.emplace_back(mlir::TupleType::get(eleTy.getContext(), range), AT{});
    return marshal;
  }
};
} // namespace

//===----------------------------------------------------------------------===//
// RISCV64 linux target specifics.
//===----------------------------------------------------------------------===//

namespace {
struct TargetRISCV64 : public GenericTarget<TargetRISCV64> {
  using GenericTarget::GenericTarget;

  static constexpr int defaultWidth = 64;

  CodeGenSpecifics::Marshalling
  complexArgumentType(mlir::Location loc, mlir::Type eleTy) const override {
    CodeGenSpecifics::Marshalling marshal;
    const auto *sem = &floatToSemantics(kindMap, eleTy);
    if (sem == &llvm::APFloat::IEEEsingle() ||
        sem == &llvm::APFloat::IEEEdouble()) {
      // Two distinct element type arguments (re, im)
      marshal.emplace_back(eleTy, AT{});
      marshal.emplace_back(eleTy, AT{});
    } else {
      TODO(loc, "complex for this precision");
    }
    return marshal;
  }

  CodeGenSpecifics::Marshalling
  complexReturnType(mlir::Location loc, mlir::Type eleTy) const override {
    CodeGenSpecifics::Marshalling marshal;
    const auto *sem = &floatToSemantics(kindMap, eleTy);
    if (sem == &llvm::APFloat::IEEEsingle() ||
        sem == &llvm::APFloat::IEEEdouble()) {
      // Use a type that will be translated into LLVM as:
      // { t, t }   struct of 2 eleTy, byVal
      mlir::TypeRange range = {eleTy, eleTy};
      marshal.emplace_back(mlir::TupleType::get(eleTy.getContext(), range),
                           AT{/*alignment=*/0, /*byval=*/true});
    } else {
      TODO(loc, "complex for this precision");
    }
    return marshal;
  }
};
} // namespace

// Instantiate the overloaded target instance based on the triple value.
// TODO: Add other targets to this file as needed.
std::unique_ptr<fir::CodeGenSpecifics>
fir::CodeGenSpecifics::get(mlir::MLIRContext *ctx, llvm::Triple &&trp,
                           KindMapping &&kindMap) {
  switch (trp.getArch()) {
  default:
    break;
  case llvm::Triple::ArchType::x86:
    if (trp.isOSWindows())
      return std::make_unique<TargetI386Win>(ctx, std::move(trp),
                                             std::move(kindMap));
    else
      return std::make_unique<TargetI386>(ctx, std::move(trp),
                                          std::move(kindMap));
  case llvm::Triple::ArchType::x86_64:
    if (trp.isOSWindows())
      return std::make_unique<TargetX86_64Win>(ctx, std::move(trp),
                                               std::move(kindMap));
    else
      return std::make_unique<TargetX86_64>(ctx, std::move(trp),
                                            std::move(kindMap));
  case llvm::Triple::ArchType::aarch64:
    return std::make_unique<TargetAArch64>(ctx, std::move(trp),
                                           std::move(kindMap));
  case llvm::Triple::ArchType::ppc64:
    return std::make_unique<TargetPPC64>(ctx, std::move(trp),
                                         std::move(kindMap));
  case llvm::Triple::ArchType::ppc64le:
    return std::make_unique<TargetPPC64le>(ctx, std::move(trp),
                                           std::move(kindMap));
  case llvm::Triple::ArchType::sparc:
    return std::make_unique<TargetSparc>(ctx, std::move(trp),
                                         std::move(kindMap));
  case llvm::Triple::ArchType::sparcv9:
    return std::make_unique<TargetSparcV9>(ctx, std::move(trp),
                                           std::move(kindMap));
  case llvm::Triple::ArchType::riscv64:
    return std::make_unique<TargetRISCV64>(ctx, std::move(trp),
                                           std::move(kindMap));
  }
  TODO(mlir::UnknownLoc::get(ctx), "target not implemented");
}
