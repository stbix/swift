//===--- PartitionUtils.h -------------------------------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2023 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_SILOPTIMIZER_UTILS_PARTITIONUTILS_H
#define SWIFT_SILOPTIMIZER_UTILS_PARTITIONUTILS_H

#include "swift/Basic/Defer.h"
#include "swift/Basic/LLVM.h"
#include "swift/SIL/SILInstruction.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/Debug.h"
#include <algorithm>

#define DEBUG_TYPE "transfer-non-sendable"

namespace swift {

namespace PartitionPrimitives {

#ifndef NDEBUG
extern bool REGIONBASEDISOLATION_ENABLE_VERBOSE_LOGGING;
#define REGIONBASEDISOLATION_VERBOSE_LOG(...)                                  \
  do {                                                                         \
    if (REGIONBASEDISOLATION_ENABLE_VERBOSE_LOGGING) {                         \
      LLVM_DEBUG(__VA_ARGS__);                                                 \
    }                                                                          \
  } while (0);
#else
#define REGIONBASEDISOLATION_VERBOSE_LOG(...)
#endif

struct Element {
  unsigned num;

  explicit Element(int num) : num(num) {}

  bool operator==(const Element &other) const { return num == other.num; }
  bool operator<(const Element &other) const { return num < other.num; }

  operator unsigned() const { return num; }
};

struct Region {
  signed num;

  explicit Region(int num) : num(num) {
    assert(num >= -1 && "-1 is the only valid negative Region label");
  }

  bool operator==(const Region &other) const { return num == other.num; }
  bool operator<(const Region &other) const { return num < other.num; }

  operator signed() const { return num; }

  bool isTransferred() const { return num < 0; }

  static Region transferred() { return Region(-1); }
};
}

using namespace PartitionPrimitives;

/// PartitionOpKind represents the different kinds of PartitionOps that
/// SILInstructions can be translated to
enum class PartitionOpKind : uint8_t {
  /// Assign one value to the region of another, takes two args, second arg
  /// must already be tracked with a non-transferred region
  Assign,

  /// Assign one value to a fresh region, takes one arg.
  AssignFresh,

  /// Merge the regions of two values, takes two args, both must be from
  /// non-transferred regions.
  Merge,

  /// Transfer the region of a value if not already transferred, takes one arg.
  Transfer,

  /// Require the region of a value to be non-transferred, takes one arg.
  Require,
};

/// PartitionOp represents a primitive operation that can be performed on
/// Partitions. This is part of the TransferNonSendable SIL pass workflow:
/// first SILBasicBlocks are compiled to vectors of PartitionOps, then a fixed
/// point partition is found over the CFG.
class PartitionOp {
private:
  PartitionOpKind OpKind;
  llvm::SmallVector<Element, 2> OpArgs;

  /// Record the SILInstruction that this PartitionOp was generated from, if
  /// generated during compilation from a SILBasicBlock
  SILInstruction *sourceInst;

  /// Record an AST expression corresponding to this PartitionOp, currently
  /// populated only for Transfer expressions to indicate the value being
  /// transferred
  Expr *sourceExpr;

  // TODO: can the following declarations be merged?
  PartitionOp(PartitionOpKind OpKind, Element arg1,
              SILInstruction *sourceInst = nullptr,
              Expr* sourceExpr = nullptr)
      : OpKind(OpKind), OpArgs({arg1}),
        sourceInst(sourceInst), sourceExpr(sourceExpr) {}

  PartitionOp(PartitionOpKind OpKind, Element arg1, Element arg2,
              SILInstruction *sourceInst = nullptr,
              Expr* sourceExpr = nullptr)
      : OpKind(OpKind), OpArgs({arg1, arg2}),
        sourceInst(sourceInst), sourceExpr(sourceExpr) {}

  friend class Partition;

public:
  static PartitionOp Assign(Element tgt, Element src,
                            SILInstruction *sourceInst = nullptr) {
    return PartitionOp(PartitionOpKind::Assign, tgt, src, sourceInst);
  }

  static PartitionOp AssignFresh(Element tgt,
                                 SILInstruction *sourceInst = nullptr) {
    return PartitionOp(PartitionOpKind::AssignFresh, tgt, sourceInst);
  }

  static PartitionOp Transfer(Element tgt, SILInstruction *sourceInst = nullptr,
                              Expr *sourceExpr = nullptr) {
    return PartitionOp(PartitionOpKind::Transfer, tgt, sourceInst, sourceExpr);
  }

  static PartitionOp Merge(Element tgt1, Element tgt2,
                           SILInstruction *sourceInst = nullptr) {
    return PartitionOp(PartitionOpKind::Merge, tgt1, tgt2, sourceInst);
  }

  static PartitionOp Require(Element tgt,
                             SILInstruction *sourceInst = nullptr) {
    return PartitionOp(PartitionOpKind::Require, tgt, sourceInst);
  }

  bool operator==(const PartitionOp &other) const {
      return OpKind == other.OpKind
             && OpArgs == other.OpArgs
                && sourceInst == other.sourceInst;
  };

  bool operator<(const PartitionOp &other) const {
    if (OpKind != other.OpKind)
      return OpKind < other.OpKind;
    if (OpArgs != other.OpArgs)
      return OpArgs < other.OpArgs;
    return sourceInst < other.sourceInst;
  }

  PartitionOpKind getKind() const { return OpKind; }

  ArrayRef<Element> getOpArgs() const { return OpArgs; }

  SILInstruction *getSourceInst(bool assertNonNull = false) const {
    assert(!assertNonNull ||
           sourceInst && "PartitionOps should be assigned SILInstruction"
                         " sources when used for the core analysis");
    return sourceInst;
  }

  Expr *getSourceExpr() const {
    return sourceExpr;
  }

  SWIFT_DEBUG_DUMP { print(llvm::dbgs()); }

  void print(llvm::raw_ostream &os) const {
    switch (OpKind) {
    case PartitionOpKind::Assign:
      os << "assign %%" << OpArgs[0] << " = %%" << OpArgs[1];
      break;
    case PartitionOpKind::AssignFresh:
      os << "assign_fresh %%" << OpArgs[0];
      break;
    case PartitionOpKind::Transfer:
      os << "transfer %%" << OpArgs[0];
      break;
    case PartitionOpKind::Merge:
      os << "merge %%" << OpArgs[0] << " with %%" << OpArgs[1];
      break;
    case PartitionOpKind::Require:
      os << "require %%" << OpArgs[0];
      break;
    }
    os << ": " << *getSourceInst(true);
  }
};

/// For the passed `map`, ensure that `key` maps to `val`. If `key` already
/// mapped to a different value, ensure that all other keys mapped to that
/// value also now map to `val`. This is a relatively expensive (linear time)
/// operation that's unfortunately used pervasively throughout PartitionOp
/// application. If this is a performance bottleneck, let's consider optimizing
/// it to a true union-find or other tree-based data structure.
static void horizontalUpdate(std::map<Element, Region> &map, Element key,
                             Region val) {
  if (!map.count(key)) {
    map.insert({key, val});
    return;
  }

  Region oldVal = map.at(key);
  if (val == oldVal)
    return;

  for (auto [otherKey, otherVal] : map)
    if (otherVal == oldVal)
      map.insert_or_assign(otherKey, val);
}

/// A map from Element -> Region that represents the current partition set.
///
///
class Partition {
public:
  /// A class defined in PartitionUtils unittest used to grab state from
  /// Partition without exposing it to other users.
  struct PartitionTester;

private:
  /// Label each index with a non-negative (unsigned) label if it is associated
  /// with a valid region, and with -1 if it is associated with a transferred
  /// region in-order traversal relied upon.
  std::map<Element, Region> labels;

  /// Track a label that is guaranteed to be strictly larger than all in use,
  /// and therefore safe for use as a fresh label.
  Region fresh_label = Region(0);

  /// In a canonical partition, all regions are labelled with the smallest index
  /// of any member. Certain operations like join and equals rely on
  /// canonicality so when it's invalidated this boolean tracks that, and it
  /// must be reestablished by a call to canonicalize().
  bool canonical;

public:
  Partition() : labels({}), canonical(true) {}

  /// 1-arg constructor used when canonicality will be immediately invalidated,
  /// so set to false to begin with
  Partition(bool canonical) : labels({}), canonical(canonical) {}

  static Partition singleRegion(ArrayRef<Element> indices) {
    Partition p;
    if (!indices.empty()) {
      Region min_index =
          Region(*std::min_element(indices.begin(), indices.end()));
      p.fresh_label = Region(min_index + 1);
      for (Element index : indices) {
        p.labels.insert_or_assign(index, min_index);
      }
    }

    assert(p.is_canonical_correct());
    return p;
  }

  static Partition separateRegions(ArrayRef<Element> indices) {
    Partition p;
    if (indices.empty())
      return p;

    auto maxIndex = Element(0);
    for (Element index : indices) {
      p.labels.insert_or_assign(index, Region(index));
      maxIndex = Element(std::max(maxIndex, index));
    }
    p.fresh_label = Region(maxIndex + 1);
    assert(p.is_canonical_correct());
    return p;
  }

  /// Test two partititons for equality by first putting them in canonical form
  /// then comparing for exact equality.
  ///
  /// Runs in linear time.
  static bool equals(Partition &fst, Partition &snd) {
    fst.canonicalize();
    snd.canonicalize();

    return fst.labels == snd.labels;
  }

  bool isTracked(Element val) const { return labels.count(val); }

  bool isTransferred(Element val) const {
    return isTracked(val) && labels.at(val).isTransferred();
  }

  /// Construct the partition corresponding to the union of the two passed
  /// partitions.
  ///
  /// Runs in quadratic time.
  static Partition join(const Partition &fst, const Partition &snd) {
    // First copy and canonicalize our inputs.
    Partition fst_reduced = fst;
    Partition snd_reduced = snd;

    fst_reduced.canonicalize();
    snd_reduced.canonicalize();

    // For each element in snd_reduced...
    for (const auto &[sndEltNumber, sndRegionNumber] : snd_reduced.labels) {
      // For values that are both in fst_reduced and snd_reduced, we need to
      // merge their regions.
      if (fst_reduced.labels.count(sndEltNumber)) {
        if (sndRegionNumber.isTransferred()) {
          // If snd says that the region has been transferred, mark it
          // transferred in fst.
          horizontalUpdate(fst_reduced.labels, sndEltNumber,
                           Region::transferred());
          continue;
        }

        // Otherwise merge. This maintains canonicality.
        fst_reduced.merge(sndEltNumber, Element(sndRegionNumber));
        continue;
      }

      // Otherwise, we have an element in snd that is not in fst. First see if
      // our region is transferred. In such a case, just add this element as
      // transferred.
      if (sndRegionNumber.isTransferred()) {
        fst_reduced.labels.insert({sndEltNumber, Region::transferred()});
        continue;
      }

      // Then check if the representative element number for this element in snd
      // is in fst. In that case, we know that we visited it before we visited
      // this elt number (since we are processing in order) so what ever is
      // mapped to that number in snd must be the correct number for this
      // element as well since this number is guaranteed to be greater than our
      // representative and the number mapped to our representative in fst must
      // be <= our representative.
      auto iter = fst_reduced.labels.find(Element(sndRegionNumber));
      if (iter != fst_reduced.labels.end()) {
        fst_reduced.labels.insert({sndEltNumber, iter->second});
        if (fst_reduced.fresh_label < Region(sndEltNumber))
          fst_reduced.fresh_label = Region(sndEltNumber + 1);
        continue;
      }

      // Otherwise, we have an element that is not in fst and its representative
      // is not in fst. This means that we must be our representative in snd
      // since we should have visited our representative earlier if we were not
      // due to our traversal being in order. Thus just add this to fst_reduced.
      assert(sndEltNumber == Element(sndRegionNumber));
      fst_reduced.labels.insert({sndEltNumber, sndRegionNumber});
      if (fst_reduced.fresh_label < sndRegionNumber)
        fst_reduced.fresh_label = Region(sndEltNumber + 1);
    }

    LLVM_DEBUG(llvm::dbgs() << "JOIN PEFORMED: \nFST: ";
               fst.print(llvm::dbgs()); llvm::dbgs() << "SND: ";
               snd.print(llvm::dbgs()); llvm::dbgs() << "RESULT: ";
               fst_reduced.print(llvm::dbgs()););

    assert(fst_reduced.is_canonical_correct());

    // fst_reduced is now the join
    return fst_reduced;
  }

  /// Apply the passed PartitionOp to this partition, performing its action.  A
  /// `handleFailure` closure can optionally be passed in that will be called if
  /// a transferred region is required. The closure is given the PartitionOp
  /// that failed, and the index of the SIL value that was required but
  /// transferred. Additionally, a list of "nontransferrable" indices can be
  /// passed in along with a handleTransferNonTransferrable closure. In the
  /// event that a region containing one of the nontransferrable indices is
  /// transferred, the closure will be called with the offending transfer.
  void apply(
      PartitionOp op,
      llvm::function_ref<void(const PartitionOp &, Element)> handleFailure =
          [](const PartitionOp &, Element) {},
      ArrayRef<Element> nontransferrables = {},
      llvm::function_ref<void(const PartitionOp &, Element)>
          handleTransferNonTransferrable = [](const PartitionOp &, Element) {},
      llvm::function_ref<bool(Element)> isActorDerived = nullptr) {

    REGIONBASEDISOLATION_VERBOSE_LOG(llvm::dbgs() << "Applying: ";
                                     op.print(llvm::dbgs()));
    REGIONBASEDISOLATION_VERBOSE_LOG(llvm::dbgs() << "    Before: ";
                                     print(llvm::dbgs()));
    SWIFT_DEFER {
      REGIONBASEDISOLATION_VERBOSE_LOG(llvm::dbgs() << "    After:  ";
                                       print(llvm::dbgs()));
    };
    switch (op.OpKind) {
    case PartitionOpKind::Assign:
      assert(op.OpArgs.size() == 2 &&
             "Assign PartitionOp should be passed 2 arguments");
      assert(labels.count(op.OpArgs[1]) &&
             "Assign PartitionOp's source argument should be already tracked");
      // if assigning to a missing region, handle the failure
      if (isTransferred(op.OpArgs[1]))
        handleFailure(op, op.OpArgs[1]);

      labels.insert_or_assign(op.OpArgs[0], labels.at(op.OpArgs[1]));

      // assignment could have invalidated canonicality of either the old region
      // of op.OpArgs[0] or the region of op.OpArgs[1], or both
      canonical = false;
      break;
    case PartitionOpKind::AssignFresh:
      assert(op.OpArgs.size() == 1 &&
             "AssignFresh PartitionOp should be passed 1 argument");

      // map index op.OpArgs[0] to a fresh label
      labels.insert_or_assign(op.OpArgs[0], fresh_label);

      // increment the fresh label so it remains fresh
      fresh_label = Region(fresh_label + 1);
      canonical = false;
      break;
    case PartitionOpKind::Transfer: {
      assert(op.OpArgs.size() == 1 &&
             "Transfer PartitionOp should be passed 1 argument");
      assert(labels.count(op.OpArgs[0]) &&
             "Transfer PartitionOp's argument should already be tracked");

      // check if any nontransferrables are transferred here, and handle the
      // failure if so
      for (Element nonTransferrable : nontransferrables) {
        assert(
            labels.count(nonTransferrable) &&
            "nontransferrables should be function args and self, and therefore"
            "always present in the label map because of initialization at "
            "entry");
        if (!isTransferred(nonTransferrable) &&
            labels.at(nonTransferrable) == labels.at(op.OpArgs[0])) {
          handleTransferNonTransferrable(op, nonTransferrable);
          break;
        }
      }

      // If this value is actor derived or if any elements in its region are
      // actor derived, we need to treat as nontransferrable.
      if (isActorDerived && isActorDerived(op.OpArgs[0]))
        return handleTransferNonTransferrable(op, op.OpArgs[0]);
      Region elementRegion = labels.at(op.OpArgs[0]);
      if (llvm::any_of(labels,
                       [&](const std::pair<Element, Region> &pair) -> bool {
                         if (pair.second != elementRegion)
                           return false;
                         return isActorDerived && isActorDerived(pair.first);
                       }))
        return handleTransferNonTransferrable(op, op.OpArgs[0]);

      // Ensure if the region is transferred...
      if (!isTransferred(op.OpArgs[0]))
        // that all elements associated with the region are marked as
        // transferred.
        horizontalUpdate(labels, op.OpArgs[0], Region::transferred());
      break;
    }
    case PartitionOpKind::Merge:
      assert(op.OpArgs.size() == 2 &&
             "Merge PartitionOp should be passed 2 arguments");
      assert(labels.count(op.OpArgs[0]) && labels.count(op.OpArgs[1]) &&
             "Merge PartitionOp's arguments should already be tracked");

      // if attempting to merge a transferred region, handle the failure
      if (isTransferred(op.OpArgs[0]))
        handleFailure(op, op.OpArgs[0]);
      if (isTransferred(op.OpArgs[1]))
        handleFailure(op, op.OpArgs[1]);

      merge(op.OpArgs[0], op.OpArgs[1]);
      break;
    case PartitionOpKind::Require:
      assert(op.OpArgs.size() == 1 &&
             "Require PartitionOp should be passed 1 argument");
      assert(labels.count(op.OpArgs[0]) &&
             "Require PartitionOp's argument should already be tracked");
      if (isTransferred(op.OpArgs[0]))
        handleFailure(op, op.OpArgs[0]);
    }

    assert(is_canonical_correct());
  }

  /// Return a vector of the transferred values in this partition.
  std::vector<Element> getTransferredVals() const {
    // For effeciency, this could return an iterator not a vector.
    std::vector<Element> transferredVals;
    for (auto [i, _] : labels)
      if (isTransferred(i))
        transferredVals.push_back(i);
    return transferredVals;
  }

  /// Return a vector of the non-transferred regions in this partition, each
  /// represented as a vector of values.
  std::vector<std::vector<Element>> getNonTransferredRegions() const {
    // For effeciency, this could return an iterator not a vector.
    std::map<Region, std::vector<Element>> buckets;

    for (auto [i, label] : labels)
      buckets[label].push_back(i);

    std::vector<std::vector<Element>> doubleVec;

    for (auto [_, bucket] : buckets)
      doubleVec.push_back(bucket);

    return doubleVec;
  }

  void dump_labels() const LLVM_ATTRIBUTE_USED {
    llvm::dbgs() << "Partition";
    if (canonical)
      llvm::dbgs() << "(canonical)";
    llvm::dbgs() << "(fresh=" << fresh_label << "){";
    for (const auto &[i, label] : labels)
      llvm::dbgs() << "[" << i << ": " << label << "] ";
    llvm::dbgs() << "}\n";
  }

  SWIFT_DEBUG_DUMP { print(llvm::dbgs()); }

  void print(llvm::raw_ostream &os) const {
    std::map<Region, std::vector<Element>> buckets;

    for (auto [i, label] : labels)
      buckets[label].push_back(i);

    os << "[";
    for (auto [label, indices] : buckets) {
      os << (label.isTransferred() ? "{" : "(");
      int j = 0;
      for (Element i : indices) {
        os << (j++ ? " " : "") << i;
      }
      os << (label.isTransferred() ? "}" : ")");
    }
    os << "]\n";
  }

private:
  /// Used only in assertions, check that Partitions promised to be canonical
  /// are actually canonical
  bool is_canonical_correct() {
    if (!canonical)
      return true; // vacuously correct

    auto fail = [&](Element i, int type) {
      llvm::dbgs() << "FAIL(i=" << i << "; type=" << type << "): ";
      print(llvm::dbgs());
      return false;
    };

    for (auto &[i, label] : labels) {
      // Correctness vacuous at transferred indices.
      if (label.isTransferred())
        continue;

      // Labels should not exceed fresh_label.
      if (label >= fresh_label)
        return fail(i, 0);

      // The label of a region should be at most as large as each index in it.
      if ((unsigned)label > i)
        return fail(i, 1);

      // Each region label should also be an element of the partition.
      if (!labels.count(Element(label)))
        return fail(i, 2);

      // Each element that is also a region label should be mapped to itself.
      if (labels.at(Element(label)) != label)
        return fail(i, 3);
    }

    return true;
  }

  /// For each region label that occurs, find the first index at which it occurs
  /// and relabel all instances of it to that index.  This excludes the -1 label
  /// for transferred regions.
  ///
  /// This runs in linear time.
  void canonicalize() {
    if (canonical)
      return;
    canonical = true;

    std::map<Region, Region> relabel;

    // relies on in-order traversal of labels
    for (auto &[i, label] : labels) {
      // leave -1 (transferred region) as is
      if (label.isTransferred())
        continue;

      if (!relabel.count(label)) {
        // if this is the first time encountering this region label,
        // then this region label should be relabelled to this index,
        // so enter that into the map
        relabel.insert_or_assign(label, Region(i));
      }

      // update this label with either its own index, or a prior index that
      // shared a region with it
      label = relabel.at(label);

      // the maximum index iterated over will be used here to appropriately
      // set fresh_label
      fresh_label = Region(i + 1);
    }

    assert(is_canonical_correct());
  }

  // linear time - merge the regions of two indices, maintaining canonicality
  void merge(Element fst, Element snd) {
    assert(labels.count(fst) && labels.count(snd));
    if (labels.at(fst) == labels.at(snd))
      return;

    // maintain canonicality by renaming the greater-numbered region
    if (labels.at(fst) < labels.at(snd))
      horizontalUpdate(labels, snd, labels.at(fst));
    else
      horizontalUpdate(labels, fst, labels.at(snd));

    assert(is_canonical_correct());
    assert(labels.at(fst) == labels.at(snd));
  }
};

} // namespace swift

#endif // SWIFT_PARTITIONUTILS_H
