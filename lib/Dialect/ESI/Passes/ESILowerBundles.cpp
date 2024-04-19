//===- ESILowerBundles.cpp - Lower ESI bundles pass -------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "../PassDetails.h"

#include "circt/Dialect/ESI/ESIOps.h"
#include "circt/Dialect/HW/HWOps.h"
#include "circt/Dialect/HW/PortConverter.h"
#include "circt/Support/BackedgeBuilder.h"
#include "circt/Support/LLVM.h"
#include "circt/Support/SymCache.h"

#include "mlir/Transforms/GreedyPatternRewriteDriver.h"
#include "llvm/ADT/StringMap.h"

namespace circt {
namespace esi {
#define GEN_PASS_DEF_LOWERESIBUNDLES
#include "circt/Dialect/ESI/ESIPasses.h.inc"
} // namespace esi
} // namespace circt

using namespace circt;
using namespace circt::esi;
using namespace circt::esi::detail;
using namespace circt::hw;

namespace {

/// Lower channel bundles into the constituent channels. The workhorse of this
/// pass. Works by adding channel ports, using [un]pack operations to recreate
/// the original value. (Pretty standard in MLIR for type conversions.) The new
/// [un]pack operations get lowered away later on.
class BundlePort : public PortConversion {
public:
  BundlePort(PortConverterImpl &converter, hw::PortInfo origPort)
      : PortConversion(converter, origPort) {}

protected:
  // Modifies the instance signals.
  void mapInputSignals(OpBuilder &b, Operation *inst, Value instValue,
                       SmallVectorImpl<Value> &newOperands,
                       ArrayRef<Backedge> newResults) override;
  void mapOutputSignals(OpBuilder &b, Operation *inst, Value instValue,
                        SmallVectorImpl<Value> &newOperands,
                        ArrayRef<Backedge> newResults) override;

  // Modifies the module ports.
  void buildInputSignals() override;
  void buildOutputSignals() override;

private:
  SmallVector<hw::PortInfo, 4> newInputChannels;
  SmallVector<hw::PortInfo, 4> newOutputChannels;
};

/// Lower arrays of channel bundles into arrays of the constituent channels.
/// Works by adding arrays of channel ports, using [un]pack operations and
/// array_get/array_create ops to recreate arrays of the original values.
class ArrayBundlePort : public PortConversion {
public:
  ArrayBundlePort(PortConverterImpl &converter, hw::PortInfo origPort)
      : PortConversion(converter, origPort) {}

protected:
  // Modifies the instance signals.
  void mapInputSignals(OpBuilder &b, Operation *inst, Value instValue,
                       SmallVectorImpl<Value> &newOperands,
                       ArrayRef<Backedge> newResults) override;
  void mapOutputSignals(OpBuilder &b, Operation *inst, Value instValue,
                        SmallVectorImpl<Value> &newOperands,
                        ArrayRef<Backedge> newResults) override;

  // Modifies the module ports.
  void buildInputSignals() override;
  void buildOutputSignals() override;

private:
  SmallVector<hw::PortInfo, 4> newInputChannels;
  SmallVector<hw::PortInfo, 4> newOutputChannels;
};

class ESIBundleConversionBuilder : public PortConversionBuilder {
public:
  using PortConversionBuilder::PortConversionBuilder;
  FailureOr<std::unique_ptr<PortConversion>> build(hw::PortInfo port) override {
    return llvm::TypeSwitch<Type, FailureOr<std::unique_ptr<PortConversion>>>(
               port.type)
        .Case([&](esi::ChannelBundleType)
                  -> FailureOr<std::unique_ptr<PortConversion>> {
          return {std::make_unique<BundlePort>(converter, port)};
        })
        .Case([&](hw::ArrayType arrayType)
                  -> FailureOr<std::unique_ptr<PortConversion>> {
          if (isa<esi::ChannelBundleType>(arrayType.getElementType()))
            return {std::make_unique<ArrayBundlePort>(converter, port)};
          return PortConversionBuilder::build(port);
        })
        .Default([&](auto) { return PortConversionBuilder::build(port); });
  }
};
} // namespace

/// When replacing an instance with an input bundle, we must unpack the
/// individual channels and feed/consume them into/from the new instance.
void BundlePort::mapInputSignals(OpBuilder &b, Operation *inst, Value,
                                 SmallVectorImpl<Value> &newOperands,
                                 ArrayRef<Backedge> newResults) {
  // Assemble the operands/result types and build the op.
  SmallVector<Value, 4> fromChannels(
      llvm::map_range(newOutputChannels, [&](hw::PortInfo port) {
        return newResults[port.argNum];
      }));
  SmallVector<Type, 5> toChannelTypes(llvm::map_range(
      newInputChannels, [](hw::PortInfo port) { return port.type; }));
  auto unpack = b.create<UnpackBundleOp>(
      origPort.loc,
      /*bundle=*/inst->getOperand(origPort.argNum), fromChannels);

  // Connect the new instance inputs to the results of the unpack.
  for (auto [idx, inPort] : llvm::enumerate(newInputChannels))
    newOperands[inPort.argNum] = unpack.getResult(idx);
}

/// When replacing an instance with an output bundle, we must pack the
/// individual channels in a bundle to recreate the original Value.
void BundlePort::mapOutputSignals(OpBuilder &b, Operation *inst, Value,
                                  SmallVectorImpl<Value> &newOperands,
                                  ArrayRef<Backedge> newResults) {
  // Assemble the operands/result types and build the op.
  SmallVector<Value, 4> toChannels(
      llvm::map_range(newOutputChannels, [&](hw::PortInfo port) {
        return newResults[port.argNum];
      }));
  SmallVector<Type, 5> fromChannelTypes(llvm::map_range(
      newInputChannels, [](hw::PortInfo port) { return port.type; }));
  auto pack = b.create<PackBundleOp>(
      origPort.loc, cast<ChannelBundleType>(origPort.type), toChannels);

  // Feed the fromChannels into the new instance.
  for (auto [idx, inPort] : llvm::enumerate(newInputChannels))
    newOperands[inPort.argNum] = pack.getFromChannels()[idx];
  // Replace the users of the old bundle Value with the new one.
  inst->getResult(origPort.argNum).replaceAllUsesWith(pack.getBundle());
}

/// When replacing an instance with an input bundle, we must unpack the
/// bundle into its individual channels.
void BundlePort::buildInputSignals() {
  auto bundleType = cast<ChannelBundleType>(origPort.type);
  SmallVector<Value, 4> newInputValues;
  SmallVector<BundledChannel, 4> outputChannels;

  for (BundledChannel ch : bundleType.getChannels()) {
    // 'to' on an input bundle becomes an input channel.
    if (ch.direction == ChannelDirection::to) {
      hw::PortInfo newPort;
      newInputValues.push_back(converter.createNewInput(
          origPort, "_" + ch.name.getValue(), ch.type, newPort));
      newInputChannels.push_back(newPort);
    } else {
      // 'from' on an input bundle becomes an output channel.
      outputChannels.push_back(ch);
    }
  }

  // On an input port, new channels must be packed to recreate the original
  // Value.
  PackBundleOp pack;
  if (body) {
    ImplicitLocOpBuilder b(origPort.loc, body, body->begin());
    pack = b.create<PackBundleOp>(bundleType, newInputValues);
    body->getArgument(origPort.argNum).replaceAllUsesWith(pack.getBundle());
  }

  // Build new ports and put the new port info directly into the member
  // variable.
  newOutputChannels.resize(outputChannels.size());
  for (auto [idx, ch] : llvm::enumerate(outputChannels))
    converter.createNewOutput(origPort, "_" + ch.name.getValue(), ch.type,
                              pack ? pack.getFromChannels()[idx] : nullptr,
                              newOutputChannels[idx]);
}

/// For an output port, we need to unpack the results from the original value
/// into the new channel ports.
void BundlePort::buildOutputSignals() {
  auto bundleType = cast<ChannelBundleType>(origPort.type);
  SmallVector<Value, 4> unpackChannels;
  SmallVector<BundledChannel, 4> outputChannels;

  SmallVector<Type, 4> unpackOpResultTypes;
  for (BundledChannel ch : bundleType.getChannels()) {
    // 'from' on an input bundle becomes an input channel.
    if (ch.direction == ChannelDirection::from) {
      hw::PortInfo newPort;
      unpackChannels.push_back(converter.createNewInput(
          origPort, "_" + ch.name.getValue(), ch.type, newPort));
      newInputChannels.push_back(newPort);
    } else {
      // 'to' on an input bundle becomes an output channel.
      unpackOpResultTypes.push_back(ch.type);
      outputChannels.push_back(ch);
    }
  }

  // For an output port, the original bundle must be unpacked into the
  // individual channel ports.
  UnpackBundleOp unpack;
  if (body)
    unpack = OpBuilder::atBlockTerminator(body).create<UnpackBundleOp>(
        origPort.loc, body->getTerminator()->getOperand(origPort.argNum),
        unpackChannels);

  // Build new ports and put the new port info directly into the member
  // variable.
  newOutputChannels.resize(outputChannels.size());
  for (auto [idx, ch] : llvm::enumerate(outputChannels))
    converter.createNewOutput(origPort, "_" + ch.name.getValue(), ch.type,
                              unpack ? unpack.getToChannels()[idx] : nullptr,
                              newOutputChannels[idx]);
}

/// When replacing an instance with an input bundle, we must unpack the
/// individual channels and feed/consume them into/from the new instance.
void ArrayBundlePort::mapInputSignals(OpBuilder &b, Operation *inst, Value,
                                      SmallVectorImpl<Value> &newOperands,
                                      ArrayRef<Backedge> newResults) {
  // Assemble the operands/result types and build the op.
  SmallVector<Value, 4> fromChannels(
      llvm::map_range(newOutputChannels, [&](hw::PortInfo port) {
        return newResults[port.argNum];
      }));
  SmallVector<Type, 5> toChannelTypes(llvm::map_range(
      newInputChannels, [](hw::PortInfo port) { return port.type; }));
  auto unpack = b.create<UnpackBundleOp>(
      origPort.loc,
      /*bundle=*/inst->getOperand(origPort.argNum), fromChannels);

  // Connect the new instance inputs to the results of the unpack.
  for (auto [idx, inPort] : llvm::enumerate(newInputChannels))
    newOperands[inPort.argNum] = unpack.getResult(idx);
}

/// When replacing an instance with an output bundle, we must pack the
/// individual channels in a bundle to recreate the original Value.
void ArrayBundlePort::mapOutputSignals(OpBuilder &b, Operation *inst, Value,
                                       SmallVectorImpl<Value> &newOperands,
                                       ArrayRef<Backedge> newResults) {
  // Assemble the operands/result types and build the op.
  SmallVector<Value, 4> toChannels(
      llvm::map_range(newOutputChannels, [&](hw::PortInfo port) {
        return newResults[port.argNum];
      }));
  SmallVector<Type, 5> fromChannelTypes(llvm::map_range(
      newInputChannels, [](hw::PortInfo port) { return port.type; }));
  auto pack = b.create<PackBundleOp>(
      origPort.loc, cast<ChannelBundleType>(origPort.type), toChannels);

  // Feed the fromChannels into the new instance.
  for (auto [idx, inPort] : llvm::enumerate(newInputChannels))
    newOperands[inPort.argNum] = pack.getFromChannels()[idx];
  // Replace the users of the old bundle Value with the new one.
  inst->getResult(origPort.argNum).replaceAllUsesWith(pack.getBundle());
}

/// When replacing an instance with an input bundle, we must unpack the
/// bundle into its individual channels.
void ArrayBundlePort::buildInputSignals() {
  auto bundleType = cast<ChannelBundleType>(origPort.type);
  SmallVector<Value, 4> newInputValues;
  SmallVector<BundledChannel, 4> outputChannels;

  for (BundledChannel ch : bundleType.getChannels()) {
    // 'to' on an input bundle becomes an input channel.
    if (ch.direction == ChannelDirection::to) {
      hw::PortInfo newPort;
      newInputValues.push_back(converter.createNewInput(
          origPort, "_" + ch.name.getValue(), ch.type, newPort));
      newInputChannels.push_back(newPort);
    } else {
      // 'from' on an input bundle becomes an output channel.
      outputChannels.push_back(ch);
    }
  }

  // On an input port, new channels must be packed to recreate the original
  // Value.
  PackBundleOp pack;
  if (body) {
    ImplicitLocOpBuilder b(origPort.loc, body, body->begin());
    pack = b.create<PackBundleOp>(bundleType, newInputValues);
    body->getArgument(origPort.argNum).replaceAllUsesWith(pack.getBundle());
  }

  // Build new ports and put the new port info directly into the member
  // variable.
  newOutputChannels.resize(outputChannels.size());
  for (auto [idx, ch] : llvm::enumerate(outputChannels))
    converter.createNewOutput(origPort, "_" + ch.name.getValue(), ch.type,
                              pack ? pack.getFromChannels()[idx] : nullptr,
                              newOutputChannels[idx]);
}

/// For an output port, we need to take each bundle in the array of bundles and
/// unpack it. Then we need to create arrays of the unpacked channels and expose
/// them as inputs or outputs as appropriate. Channels in the 'to' direction
/// need to become arrays of channels being output. Channels in the 'from'
/// direction need to become arrays of channels as inputs.
void ArrayBundlePort::buildOutputSignals() {
  auto bundleType = cast<ChannelBundleType>(
      cast<hw::ArrayType>(origPort.type).getElementType());
  size_t numBundles = cast<hw::ArrayType>(origPort.type).getNumElements();
  SmallVector<BundledChannel, 4> toChannelTypes(
      llvm::make_filter_range(bundleType.getChannels(), [](BundledChannel ch) {
        return ch.direction == ChannelDirection::to;
      }));
  SmallVector<BundledChannel, 4> fromChannelTypes(
      llvm::make_filter_range(bundleType.getChannels(), [](BundledChannel ch) {
        return ch.direction == ChannelDirection::from;
      }));

  std::optional<ImplicitLocOpBuilder> builder;
  if (body)
    builder =
        ImplicitLocOpBuilder(origPort.loc, OpBuilder::atBlockTerminator(body));

  // *****
  // Construct the new input channels

  // List of channels indexed by bundle number. Allocate space for each bundle.
  SmallVector<SmallVector<Value>, 4> fromChannels(numBundles);

  // For each 'from' channel, add an input port. If there is a body, for each
  // bundle in the original array, get the original channel.
  for (BundledChannel ch : fromChannelTypes) {
    auto chanArrayTy = hw::ArrayType::get(ch.type, numBundles);
    hw::PortInfo newPort;
    auto arrayOfChannels = converter.createNewInput(
        origPort, "_" + ch.name.getValue(), chanArrayTy, newPort);

    if (!body)
      continue;
    for (size_t bundleNum = 0; bundleNum < numBundles; ++bundleNum) {
      Value channel =
          builder->create<hw::ArrayGetOp>(arrayOfChannels, bundleNum);
      fromChannels[bundleNum].push_back(channel);
    }
  }

  // *****
  // Unpack the channels.

  // New arrays of channels indexed by channel number.
  SmallVector<mlir::TypedValue<hw::ArrayType>> outputChannelArrays;

  if (body) {
    // List of 'to' channels indexed by channel index.
    SmallVector<SmallVector<Value>, 4> toChannels(toChannelTypes.size());
    // The original array of bundles driving the output port.
    Value origBundleArray = body->getTerminator()->getOperand(origPort.argNum);

    for (size_t bundleNum = 0; bundleNum < numBundles; ++bundleNum) {
      auto bundle = builder->create<hw::ArrayGetOp>(origBundleArray, bundleNum);
      auto unpackedToChannelsForBundle =
          builder->create<UnpackBundleOp>(bundle, fromChannels[bundleNum])
              .getToChannels();
      for (auto [idx, toChannel] : llvm::enumerate(unpackedToChannelsForBundle))
        toChannels[idx].push_back(toChannel);
    }

    for (auto channelArrayList : toChannels)
      outputChannelArrays.push_back(
          builder->create<hw::ArrayCreateOp>(channelArrayList)
              .getResult()
              .cast<mlir::TypedValue<hw::ArrayType>>());
  }

  // *****
  // Build new ports and put the new port info directly into the member
  // variable.
  newOutputChannels.resize(toChannelTypes.size());
  for (auto [idx, ch] : llvm::enumerate(toChannelTypes))
    converter.createNewOutput(
        origPort, "_" + ch.name.getValue(), ArrayType::get(ch.type, numBundles),
        body ? outputChannelArrays[idx] : nullptr, newOutputChannels[idx]);
}

namespace {
/// Convert all the ESI bundle ports on modules to channel ports.
struct ESIBundlesPass
    : public circt::esi::impl::LowerESIBundlesBase<ESIBundlesPass> {
  void runOnOperation() override;
};
} // anonymous namespace

/// Iterate through the `hw.module[.extern]`s and lower their ports.
void ESIBundlesPass::runOnOperation() {
  MLIRContext &ctxt = getContext();
  ModuleOp top = getOperation();

  // Find all modules and run port conversion on them.
  circt::hw::InstanceGraph &instanceGraph =
      getAnalysis<circt::hw::InstanceGraph>();
  for (auto mod : top.getOps<HWMutableModuleLike>()) {
    if (failed(PortConverter<ESIBundleConversionBuilder>(instanceGraph, mod)
                   .run()))
      return signalPassFailure();
  }

  // Canonicalize away bundle packs and unpacks. Any non-back-to-back
  // [un]packs need to be gone by now.
  RewritePatternSet patterns(&ctxt);
  PackBundleOp::getCanonicalizationPatterns(patterns, &ctxt);
  UnpackBundleOp::getCanonicalizationPatterns(patterns, &ctxt);
  if (failed(mlir::applyPatternsAndFoldGreedily(getOperation(),
                                                std::move(patterns))))
    signalPassFailure();

  top.walk([&](PackBundleOp pack) {
    pack.emitError("PackBundleOp should have been canonicalized away by now");
    signalPassFailure();
  });
}

std::unique_ptr<OperationPass<ModuleOp>>
circt::esi::createESIBundleLoweringPass() {
  return std::make_unique<ESIBundlesPass>();
}
