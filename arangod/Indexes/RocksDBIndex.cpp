////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2014-2016 ArangoDB GmbH, Cologne, Germany
/// Copyright 2004-2014 triAGENS GmbH, Cologne, Germany
///
/// Licensed under the Apache License, Version 2.0 (the "License");
/// you may not use this file except in compliance with the License.
/// You may obtain a copy of the License at
///
///     http://www.apache.org/licenses/LICENSE-2.0
///
/// Unless required by applicable law or agreed to in writing, software
/// distributed under the License is distributed on an "AS IS" BASIS,
/// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
/// See the License for the specific language governing permissions and
/// limitations under the License.
///
/// Copyright holder is ArangoDB GmbH, Cologne, Germany
///
/// @author Jan Steemann
////////////////////////////////////////////////////////////////////////////////

#include "RocksDBIndex.h"
#include "Aql/AstNode.h"
#include "Aql/SortCondition.h"
#include "Basics/AttributeNameParser.h"
#include "Basics/debugging.h"
#include "Basics/VelocyPackHelper.h"
#include "Indexes/PrimaryIndex.h"
#include "Indexes/RocksDBKeyComparator.h"
#include "VocBase/document-collection.h"

#include <iostream>
#include <velocypack/Iterator.h>
#include <velocypack/velocypack-aliases.h>

using namespace arangodb;

static size_t sortWeight(arangodb::aql::AstNode const* node) {
  switch (node->type) {
    case arangodb::aql::NODE_TYPE_OPERATOR_BINARY_EQ:
      return 1;
    case arangodb::aql::NODE_TYPE_OPERATOR_BINARY_IN:
      return 2;
    case arangodb::aql::NODE_TYPE_OPERATOR_BINARY_LT:
      return 3;
    case arangodb::aql::NODE_TYPE_OPERATOR_BINARY_GT:
      return 4;
    case arangodb::aql::NODE_TYPE_OPERATOR_BINARY_LE:
      return 5;
    case arangodb::aql::NODE_TYPE_OPERATOR_BINARY_GE:
      return 6;
    default:
      return 42;
  }
}

// .............................................................................
// recall for all of the following comparison functions:
//
// left < right  return -1
// left > right  return  1
// left == right return  0
//
// furthermore:
//
// the following order is currently defined for placing an order on documents
// undef < null < boolean < number < strings < lists < hash arrays
// note: undefined will be treated as NULL pointer not NULL JSON OBJECT
// within each type class we have the following order
// boolean: false < true
// number: natural order
// strings: lexicographical
// lists: lexicographically and within each slot according to these rules.
// ...........................................................................
  
RocksDBIterator::RocksDBIterator(arangodb::Transaction* trx, 
                                 arangodb::RocksDBIndex const* index,
                                 arangodb::PrimaryIndex* primaryIndex,
                                 rocksdb::DB* db,
                                 bool reverse, arangodb::velocypack::Slice const& left,
                                 arangodb::velocypack::Slice const& right)
    : _trx(trx),
      _primaryIndex(primaryIndex),
      _db(db),
      _leftEndpoint(nullptr),
      _rightEndpoint(nullptr),
      _reverse(reverse) {

  TRI_idx_iid_t const id = index->id();

  _leftEndpoint = new arangodb::velocypack::Buffer<char>();
  _leftEndpoint->append(reinterpret_cast<char const*>(&id), sizeof(TRI_idx_iid_t));
  _leftEndpoint->append(left.startAs<char const>(), left.byteSize());
 
  std::cout << "LOOKUP\n"; 
  std::cout << "SEARCHING. LEFT: (" << std::to_string(_leftEndpoint->size()) << ")\n";
  for (size_t j = 0; j < _leftEndpoint->size(); ++j) {
    std::cout << std::hex << (int) _leftEndpoint->data()[j] << " ";
  }
  std::cout << "\n";
   
  _rightEndpoint = new arangodb::velocypack::Buffer<char>();
  _rightEndpoint->append(reinterpret_cast<char const*>(&id), sizeof(TRI_idx_iid_t));
  _rightEndpoint->append(right.startAs<char const>(), right.byteSize());
  
  std::cout << "SEARCHING. RIGHT: (" << std::to_string(_rightEndpoint->size()) << ")\n";
  for (size_t j = 0; j < _rightEndpoint->size(); ++j) {
    std::cout << std::hex << (int) _rightEndpoint->data()[j] << " ";
  }
  std::cout << "\n";

  std::cout << "CREATING CURSOR\n";
  _cursor.reset(_db->NewIterator(rocksdb::ReadOptions()));

  reset();
}

////////////////////////////////////////////////////////////////////////////////
/// @brief Reset the cursor
////////////////////////////////////////////////////////////////////////////////

void RocksDBIterator::reset() {
  if (_reverse) {
    _cursor->Seek(rocksdb::Slice(_rightEndpoint->data(), _rightEndpoint->size()));
  } else {
    _cursor->Seek(rocksdb::Slice(_leftEndpoint->data(), _leftEndpoint->size()));
  }
}

////////////////////////////////////////////////////////////////////////////////
/// @brief Get the next element in the index
////////////////////////////////////////////////////////////////////////////////

TRI_doc_mptr_t* RocksDBIterator::next() {
  // TODO: append document key to make entries unambiguous
  auto comparator = _db->GetOptions().comparator;

  while (true) {
    std::cout << "COMPARE ITERATION\n";
    if (!_cursor->Valid()) {
      // We are exhausted already, sorry
  std::cout << "- CURSOR INVALID\n"; 
      return nullptr;
    }
  
  rocksdb::Slice key = _cursor->key();
  std::cout << "KEY: (" << key.size() << ")\n";
  for (size_t j = 0; j < key.size(); ++j) {
    std::cout << std::hex << (int) key[j] << " ";
  }
  std::cout << "\n";
  
  std::cout << "- FIRST COMP. KEY SIZE: " << key.size() << ", LEP SIZE: " << _leftEndpoint->size() << "\n";
    int res = comparator->Compare(key, rocksdb::Slice(_leftEndpoint->data(), _leftEndpoint->size()));
  std::cout << "- FIRST COMP DONE. RES: " << res << "\n";

    if (res < 0) {
  std::cout << "- FIRST COMP DONE. LEFT < RIGHT\n";
      if (_reverse) {
        _cursor->Prev();
      } else {
        _cursor->Next();
      }
      continue;
    }
  std::cout << "- FIRST COMP DONE. LEFT <= RIGHT\n";
  
    int res2 = comparator->Compare(key, rocksdb::Slice(_rightEndpoint->data(), _rightEndpoint->size()));
  std::cout << "- SECOND COMP DONE. RES: " << res2 << "\n";
    if (res2 > 0) {
  std::cout << "- SECOND COMP DONE. RIGHT > LEFT\n";
      return nullptr;
    }
  std::cout << "SECOND COMP DONE. RIGHT <= LEFT\n";

    break;
  }

  std::cout << "- FOUND A MATCH\n";

  rocksdb::Slice value = _cursor->value();
  _cursor->Next();
  
  std::cout << "MATCH VALUE: " << VPackSlice(value.data()).toJson() << "\n";

  // use primary index to lookup the document
  return _primaryIndex->lookupKey(_trx, VPackSlice(value.data()));
}

////////////////////////////////////////////////////////////////////////////////
/// @brief create the index
////////////////////////////////////////////////////////////////////////////////

RocksDBIndex::RocksDBIndex(
    TRI_idx_iid_t iid, TRI_document_collection_t* collection,
    std::vector<std::vector<arangodb::basics::AttributeName>> const& fields,
    bool unique, bool sparse)
    : PathBasedIndex(iid, collection, fields, unique, sparse, true),
      _db(RocksDBFeature::instance()->db()) {
}

////////////////////////////////////////////////////////////////////////////////
/// @brief create an index stub with a hard-coded selectivity estimate
/// this is used in the cluster coordinator case
////////////////////////////////////////////////////////////////////////////////

RocksDBIndex::RocksDBIndex(VPackSlice const& slice)
    : PathBasedIndex(slice, true),
      _db(nullptr) {}

////////////////////////////////////////////////////////////////////////////////
/// @brief destroy the index
////////////////////////////////////////////////////////////////////////////////

RocksDBIndex::~RocksDBIndex() {}

size_t RocksDBIndex::memory() const {
  return 0; // TODO
//  return _skiplistIndex->memoryUsage() +
//         static_cast<size_t>(_skiplistIndex->getNrUsed()) * elementSize();
}

////////////////////////////////////////////////////////////////////////////////
/// @brief return a VelocyPack representation of the index
////////////////////////////////////////////////////////////////////////////////

void RocksDBIndex::toVelocyPack(VPackBuilder& builder,
                                bool withFigures) const {
  Index::toVelocyPack(builder, withFigures);
  builder.add("unique", VPackValue(_unique));
  builder.add("sparse", VPackValue(_sparse));
}

////////////////////////////////////////////////////////////////////////////////
/// @brief return a VelocyPack representation of the index figures
////////////////////////////////////////////////////////////////////////////////

void RocksDBIndex::toVelocyPackFigures(VPackBuilder& builder) const {
  TRI_ASSERT(builder.isOpenObject());
  builder.add("memory", VPackValue(memory()));
}

////////////////////////////////////////////////////////////////////////////////
/// @brief inserts a document into the index
////////////////////////////////////////////////////////////////////////////////

int RocksDBIndex::insert(arangodb::Transaction*, TRI_doc_mptr_t const* doc,
                         bool) {
  // TODO: append document key to make entries unambiguous
  std::vector<TRI_index_element_t*> elements;

  int res;
  try {
    res = fillElement(elements, doc);
  } catch (...) {
    res = TRI_ERROR_OUT_OF_MEMORY;
  }

  if (res != TRI_ERROR_NO_ERROR) {
    for (auto& it : elements) {
      // free all elements to prevent leak
      TRI_index_element_t::freeElement(it);
    }

    return res;
  }

  VPackBuilder builder;
  std::vector<std::string> values;
  for (auto& it : elements) {
    builder.clear();
    builder.openArray();
    for (size_t i = 0; i < _fields.size(); ++i) {
      builder.add(it->subObjects()[i].slice(doc));
    }
    builder.close();
    VPackSlice const s = builder.slice();
    std::string value(reinterpret_cast<char const*>(&_iid), sizeof(TRI_idx_iid_t));
    value.append(s.startAs<char const>(), s.byteSize());
    values.emplace_back(std::move(value));
  }

  VPackSlice const key = VPackSlice(doc->vpack()).get(TRI_VOC_ATTRIBUTE_KEY);
  std::string const keyString(key.startAs<char const>(), key.byteSize());

  rocksdb::ReadOptions readOptions;
  rocksdb::WriteOptions writeOptions;
  writeOptions.sync = false;

  size_t const count = elements.size();

  for (size_t i = 0; i < count; ++i) {
    if (_unique) {
      std::string existing;
      auto status = _db->Get(readOptions, values[i], &existing); 

      if (status.ok()) {
        // duplicate key
        res = TRI_ERROR_ARANGO_UNIQUE_CONSTRAINT_VIOLATED;
      }
    }

    if (res == TRI_ERROR_NO_ERROR) {
  std::cout << "INSERTING: (" << std::to_string(values[i].size()) << ")\n";
  for (size_t j = 0; j < values[i].size(); ++j) {
    std::cout << std::hex << (int) values[i][j] << " ";
  }

  std::cout << "\n";
      auto status = _db->Put(writeOptions, values[i], keyString);
      if (! status.ok()) {
        res = TRI_ERROR_INTERNAL;
      }
    }

    if (res != TRI_ERROR_NO_ERROR) {
      for (size_t j = 0; j < i; ++j) {
        _db->Delete(writeOptions, values[i]);
      }
    
      if (res == TRI_ERROR_ARANGO_UNIQUE_CONSTRAINT_VIOLATED && !_unique) {
        // We ignore unique_constraint violated if we are not unique
        res = TRI_ERROR_NO_ERROR;
      }
      break;
    }
  }
      
  for (size_t i = 0; i < count; ++i) {
    TRI_index_element_t::freeElement(elements[i]);
  }
  return res;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief removes a document from the index
////////////////////////////////////////////////////////////////////////////////

int RocksDBIndex::remove(arangodb::Transaction*, TRI_doc_mptr_t const* doc,
                         bool) {
  // TODO: append document key to make entries unambiguous
  std::vector<TRI_index_element_t*> elements;

  int res;
  try {
    res = fillElement(elements, doc);
  } catch (...) {
    res = TRI_ERROR_OUT_OF_MEMORY;
  }

  if (res != TRI_ERROR_NO_ERROR) {
    for (auto& it : elements) {
      // free all elements to prevent leak
      TRI_index_element_t::freeElement(it);
    }

    return res;
  }
  
  VPackBuilder builder;
  std::vector<std::string> values;
  for (auto& it : elements) {
    builder.clear();
    builder.openArray();
    for (size_t i = 0; i < _fields.size(); ++i) {
      builder.add(it->subObjects()[i].slice(doc));
    }
    builder.close();
    VPackSlice const s = builder.slice();
    std::string value(reinterpret_cast<char const*>(&_iid), sizeof(TRI_idx_iid_t));
    value.append(s.startAs<char const>(), s.byteSize());
    values.emplace_back(std::move(value));
  }

  VPackSlice const key = VPackSlice(doc->vpack()).get(TRI_VOC_ATTRIBUTE_KEY);
  std::string const keyString(key.startAs<char const>(), key.byteSize());

  size_t const count = elements.size();

  for (size_t i = 0; i < count; ++i) {
    auto status = _db->Delete(rocksdb::WriteOptions(), values[i]);

    // we may be looping through this multiple times, and if an error
    // occurs, we want to keep it
    if (! status.ok()) {
      res = TRI_ERROR_INTERNAL;
    }

    TRI_index_element_t::freeElement(elements[i]);
  }

  return res;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief attempts to locate an entry in the index
///
/// Warning: who ever calls this function is responsible for destroying
/// the RocksDBIterator* results
////////////////////////////////////////////////////////////////////////////////

RocksDBIterator* RocksDBIndex::lookup(arangodb::Transaction* trx,
                                      VPackSlice const searchValues,
                                      bool reverse) const {
  TRI_ASSERT(searchValues.isArray());
  TRI_ASSERT(searchValues.length() <= _fields.size());

  VPackBuilder leftSearch;
  VPackBuilder rightSearch;
  VPackSlice lastNonEq;
  leftSearch.openArray();
  for (auto const& it : VPackArrayIterator(searchValues)) {
    TRI_ASSERT(it.isObject());
    VPackSlice eq = it.get(TRI_SLICE_KEY_EQUAL);
    if (eq.isNone()) {
      lastNonEq = it;
      break;
    }
std::cout << "FOUND TYPE: " << eq.typeName() << "\n";
    leftSearch.add(eq);
  }

  VPackSlice leftBorder;
  VPackSlice rightBorder;

  if (lastNonEq.isNone()) {
    // We only have equality!
    leftSearch.close();

std::cout << "ALL EQ\n";
    leftBorder = leftSearch.slice();
    rightBorder = leftSearch.slice();
  } else {
    // Copy rightSearch = leftSearch for right border
    rightSearch = leftSearch;

    // Define Lower-Bound 
    VPackSlice lastLeft = lastNonEq.get(TRI_SLICE_KEY_GE);
    if (!lastLeft.isNone()) {
      TRI_ASSERT(!lastNonEq.hasKey(TRI_SLICE_KEY_GT));
      leftSearch.add(lastLeft);
      leftSearch.close();
      VPackSlice search = leftSearch.slice();
      leftBorder = search;
      // leftKeyLookup guarantees that we find the element before search. This
      // should not be in the cursor, but the next one
      // This is also save for the startNode, it should never be contained in the index.
//      leftBorder = leftBorder->nextNode();
      // TODO: inc by one
    } else {
      lastLeft = lastNonEq.get(TRI_SLICE_KEY_GT);
      if (!lastLeft.isNone()) {
        leftSearch.add(lastLeft);
        leftSearch.close();
        VPackSlice search = leftSearch.slice();

        leftBorder = search;
        // leftBorder is identical or smaller than search, skip it.
        // It is guaranteed that the next element is greater than search
//        leftBorder = leftBorder->nextNode();
        // TODO: inc by one
      } else {
        // No lower bound set default to (null <= x)
        leftSearch.close();
        VPackSlice search = leftSearch.slice();
        leftBorder = search;
//        leftBorder = leftBorder->nextNode();
        // Now this is the correct leftBorder.
        // It is either the first equal one, or the first one greater than.
        // TODO: inc by one
      }
    }
    // NOTE: leftBorder could be nullptr (no element fulfilling condition.)
    // This is checked later

    // Define upper-bound

    VPackSlice lastRight = lastNonEq.get(TRI_SLICE_KEY_LE);
    if (!lastRight.isNone()) {
      TRI_ASSERT(!lastNonEq.hasKey(TRI_SLICE_KEY_LT));
      rightSearch.add(lastRight);
      rightSearch.close();
      VPackSlice search = rightSearch.slice();
      rightBorder = search;
    } else {
      lastRight = lastNonEq.get(TRI_SLICE_KEY_LT);
      if (!lastRight.isNone()) {
        rightSearch.add(lastRight);
        rightSearch.close();
        VPackSlice search = rightSearch.slice();
        rightBorder = search;
      } else {
        // No upper bound set default to (x <= INFINITY)
        rightSearch.close();
        VPackSlice search = rightSearch.slice();
        rightBorder = search;
      }
    }
  }

  auto iterator = std::make_unique<RocksDBIterator>(trx, this, _collection->primaryIndex(), _db, reverse, leftBorder, rightBorder);

  return iterator.release();
}

bool RocksDBIndex::accessFitsIndex(
    arangodb::aql::AstNode const* access, arangodb::aql::AstNode const* other,
    arangodb::aql::AstNode const* op, arangodb::aql::Variable const* reference,
    std::unordered_map<size_t, std::vector<arangodb::aql::AstNode const*>>&
        found,
    bool isExecution) const {
  if (!this->canUseConditionPart(access, other, op, reference, isExecution)) {
    return false;
  }

  arangodb::aql::AstNode const* what = access;
  std::pair<arangodb::aql::Variable const*,
            std::vector<arangodb::basics::AttributeName>> attributeData;

  if (op->type != arangodb::aql::NODE_TYPE_OPERATOR_BINARY_IN) {
    if (!what->isAttributeAccessForVariable(attributeData) ||
        attributeData.first != reference) {
      // this access is not referencing this collection
      return false;
    }
    if (arangodb::basics::TRI_AttributeNamesHaveExpansion(
            attributeData.second)) {
      // doc.value[*] == 'value'
      return false;
    }
    if (isAttributeExpanded(attributeData.second)) {
      // doc.value == 'value' (with an array index)
      return false;
    }
  } else {
    // ok, we do have an IN here... check if it's something like 'value' IN
    // doc.value[*]
    TRI_ASSERT(op->type == arangodb::aql::NODE_TYPE_OPERATOR_BINARY_IN);
    bool canUse = false;

    if (what->isAttributeAccessForVariable(attributeData) &&
        attributeData.first == reference &&
        !arangodb::basics::TRI_AttributeNamesHaveExpansion(
            attributeData.second) &&
        attributeMatches(attributeData.second)) {
      // doc.value IN 'value'
      // can use this index
      canUse = true;
    } else {
      // check for  'value' IN doc.value  AND  'value' IN doc.value[*]
      what = other;
      if (what->isAttributeAccessForVariable(attributeData) &&
          attributeData.first == reference &&
          isAttributeExpanded(attributeData.second) &&
          attributeMatches(attributeData.second)) {
        canUse = true;
      }
    }

    if (!canUse) {
      return false;
    }
  }

  std::vector<arangodb::basics::AttributeName> const& fieldNames =
      attributeData.second;

  for (size_t i = 0; i < _fields.size(); ++i) {
    if (_fields[i].size() != fieldNames.size()) {
      // attribute path length differs
      continue;
    }

    if (this->isAttributeExpanded(i) &&
        op->type != arangodb::aql::NODE_TYPE_OPERATOR_BINARY_IN) {
      // If this attribute is correct or not, it could only serve for IN
      continue;
    }

    bool match = arangodb::basics::AttributeName::isIdentical(_fields[i],
                                                              fieldNames, true);

    if (match) {
      // mark ith attribute as being covered
      auto it = found.find(i);

      if (it == found.end()) {
        found.emplace(i, std::vector<arangodb::aql::AstNode const*>{op});
      } else {
        (*it).second.emplace_back(op);
      }
      TRI_IF_FAILURE("RocksDBIndex::accessFitsIndex") {
        THROW_ARANGO_EXCEPTION(TRI_ERROR_DEBUG);
      }

      return true;
    }
  }

  return false;
}

void RocksDBIndex::matchAttributes(
    arangodb::aql::AstNode const* node,
    arangodb::aql::Variable const* reference,
    std::unordered_map<size_t, std::vector<arangodb::aql::AstNode const*>>&
        found,
    size_t& values, bool isExecution) const {
  for (size_t i = 0; i < node->numMembers(); ++i) {
    auto op = node->getMember(i);

    switch (op->type) {
      case arangodb::aql::NODE_TYPE_OPERATOR_BINARY_EQ:
      case arangodb::aql::NODE_TYPE_OPERATOR_BINARY_LT:
      case arangodb::aql::NODE_TYPE_OPERATOR_BINARY_LE:
      case arangodb::aql::NODE_TYPE_OPERATOR_BINARY_GT:
      case arangodb::aql::NODE_TYPE_OPERATOR_BINARY_GE:
        TRI_ASSERT(op->numMembers() == 2);
        accessFitsIndex(op->getMember(0), op->getMember(1), op, reference,
                        found, isExecution);
        accessFitsIndex(op->getMember(1), op->getMember(0), op, reference,
                        found, isExecution);
        break;

      case arangodb::aql::NODE_TYPE_OPERATOR_BINARY_IN:
        if (accessFitsIndex(op->getMember(0), op->getMember(1), op, reference,
                            found, isExecution)) {
          auto m = op->getMember(1);
          if (m->isArray() && m->numMembers() > 1) {
            // attr IN [ a, b, c ]  =>  this will produce multiple items, so
            // count them!
            values += m->numMembers() - 1;
          }
        }
        break;

      default:
        break;
    }
  }
}

bool RocksDBIndex::supportsFilterCondition(
    arangodb::aql::AstNode const* node,
    arangodb::aql::Variable const* reference, size_t itemsInIndex,
    size_t& estimatedItems, double& estimatedCost) const {
  std::unordered_map<size_t, std::vector<arangodb::aql::AstNode const*>> found;
  size_t values = 0;
  matchAttributes(node, reference, found, values, false);

  bool lastContainsEquality = true;
  size_t attributesCovered = 0;
  size_t attributesCoveredByEquality = 0;
  double equalityReductionFactor = 20.0;
  estimatedCost = static_cast<double>(itemsInIndex);

  for (size_t i = 0; i < _fields.size(); ++i) {
    auto it = found.find(i);

    if (it == found.end()) {
      // index attribute not covered by condition
      break;
    }

    // check if the current condition contains an equality condition
    auto const& nodes = (*it).second;
    bool containsEquality = false;
    for (size_t j = 0; j < nodes.size(); ++j) {
      if (nodes[j]->type == arangodb::aql::NODE_TYPE_OPERATOR_BINARY_EQ ||
          nodes[j]->type == arangodb::aql::NODE_TYPE_OPERATOR_BINARY_IN) {
        containsEquality = true;
        break;
      }
    }

    if (!lastContainsEquality) {
      // unsupported condition. must abort
      break;
    }

    ++attributesCovered;
    if (containsEquality) {
      ++attributesCoveredByEquality;
      estimatedCost /= equalityReductionFactor;

      // decrease the effect of the equality reduction factor
      equalityReductionFactor *= 0.25;
      if (equalityReductionFactor < 2.0) {
        // equalityReductionFactor shouldn't get too low
        equalityReductionFactor = 2.0;
      }
    } else {
      // quick estimate for the potential reductions caused by the conditions
      if (nodes.size() >= 2) {
        // at least two (non-equality) conditions. probably a range with lower
        // and upper bound defined
        estimatedCost /= 7.5;
      } else {
        // one (non-equality). this is either a lower or a higher bound
        estimatedCost /= 2.0;
      }
    }

    lastContainsEquality = containsEquality;
  }

  if (values == 0) {
    values = 1;
  }

  if (attributesCoveredByEquality == _fields.size() && unique()) {
    // index is unique and condition covers all attributes by equality
    if (estimatedItems >= values) {
      // reduce costs due to uniqueness
      estimatedItems = values;
      estimatedCost = static_cast<double>(estimatedItems);
    } else {
      // cost is already low... now slightly prioritize the unique index
      estimatedCost *= 0.995;
    }
    return true;
  }

  if (attributesCovered > 0 &&
      (!_sparse || (_sparse && attributesCovered == _fields.size()))) {
    // if the condition contains at least one index attribute and is not sparse,
    // or the index is sparse and all attributes are covered by the condition,
    // then it can be used (note: additional checks for condition parts in
    // sparse indexes are contained in Index::canUseConditionPart)
    estimatedItems = static_cast<size_t>((std::max)(
        static_cast<size_t>(estimatedCost * values), static_cast<size_t>(1)));
    estimatedCost *= static_cast<double>(values);
    return true;
  }

  // no condition
  estimatedItems = itemsInIndex;
  estimatedCost = static_cast<double>(estimatedItems);
  return false;
}

bool RocksDBIndex::supportsSortCondition(
    arangodb::aql::SortCondition const* sortCondition,
    arangodb::aql::Variable const* reference, size_t itemsInIndex,
    double& estimatedCost, size_t& coveredAttributes) const {
  TRI_ASSERT(sortCondition != nullptr);

  if (!_sparse) {
    // only non-sparse indexes can be used for sorting
    if (!_useExpansion && sortCondition->isUnidirectional() &&
        sortCondition->isOnlyAttributeAccess()) {
      coveredAttributes = sortCondition->coveredAttributes(reference, _fields);

      if (coveredAttributes >= sortCondition->numAttributes()) {
        // sort is fully covered by index. no additional sort costs!
        estimatedCost = 0.0;
        return true;
      } else if (coveredAttributes > 0) {
        estimatedCost = (itemsInIndex / coveredAttributes) *
                        std::log2(static_cast<double>(itemsInIndex));
        return true;
      }
    }
  }

  coveredAttributes = 0;
  // by default no sort conditions are supported
  if (itemsInIndex > 0) {
    estimatedCost = itemsInIndex * std::log2(static_cast<double>(itemsInIndex));
  } else {
    estimatedCost = 0.0;
  }
  return false;
}

IndexIterator* RocksDBIndex::iteratorForCondition(
    arangodb::Transaction* trx, IndexIteratorContext* context,
    arangodb::aql::Ast* ast, arangodb::aql::AstNode const* node,
    arangodb::aql::Variable const* reference, bool reverse) const {
  VPackBuilder searchValues;
  searchValues.openArray();
  bool needNormalize = false;
  if (node == nullptr) {
    // We only use this index for sort. Empty searchValue
    VPackArrayBuilder guard(&searchValues);

    TRI_IF_FAILURE("RocksDBIndex::noSortIterator") {
      THROW_ARANGO_EXCEPTION(TRI_ERROR_DEBUG);
    }
  } else {
    // Create the search Values for the lookup
    VPackArrayBuilder guard(&searchValues);

    std::unordered_map<size_t, std::vector<arangodb::aql::AstNode const*>> found;
    size_t unused = 0;
    matchAttributes(node, reference, found, unused, true);

    // found contains all attributes that are relevant for this node.
    // It might be less than fields().
    //
    // Handle the first attributes. They can only be == or IN and only
    // one node per attribute

    auto getValueAccess = [&](arangodb::aql::AstNode const* comp,
                              arangodb::aql::AstNode const*& access,
                              arangodb::aql::AstNode const*& value) -> bool {
      access = comp->getMember(0);
      value = comp->getMember(1);
      std::pair<arangodb::aql::Variable const*,
                std::vector<arangodb::basics::AttributeName>> paramPair;
      if (!(access->isAttributeAccessForVariable(paramPair) &&
            paramPair.first == reference)) {
        access = comp->getMember(1);
        value = comp->getMember(0);
        if (!(access->isAttributeAccessForVariable(paramPair) &&
              paramPair.first == reference)) {
          // Both side do not have a correct AttributeAccess, this should not
          // happen and indicates
          // an error in the optimizer
          TRI_ASSERT(false);
        }
        return true;
      }
      return false;
    };

    size_t usedFields = 0;
    for (; usedFields < _fields.size(); ++usedFields) {
      auto it = found.find(usedFields);
      if (it == found.end()) {
        // We are either done
        // or this is a range.
        // Continue with more complicated loop
        break;
      }

      auto comp = it->second[0];
      TRI_ASSERT(comp->numMembers() == 2);
      arangodb::aql::AstNode const* access = nullptr;
      arangodb::aql::AstNode const* value = nullptr;
      getValueAccess(comp, access, value);
      // We found an access for this field
      
      if (comp->type == arangodb::aql::NODE_TYPE_OPERATOR_BINARY_EQ) {
        searchValues.openObject();
        searchValues.add(VPackValue(TRI_SLICE_KEY_EQUAL));
        TRI_IF_FAILURE("RocksDBIndex::permutationEQ") {
          THROW_ARANGO_EXCEPTION(TRI_ERROR_DEBUG);
        }
      } else if (comp->type == arangodb::aql::NODE_TYPE_OPERATOR_BINARY_IN) {
        if (isAttributeExpanded(usedFields)) {
          searchValues.openObject();
          searchValues.add(VPackValue(TRI_SLICE_KEY_EQUAL));
          TRI_IF_FAILURE("RocksDBIndex::permutationArrayIN") {
            THROW_ARANGO_EXCEPTION(TRI_ERROR_DEBUG);
          }
        } else {
          needNormalize = true;
          searchValues.openObject();
          searchValues.add(VPackValue(TRI_SLICE_KEY_IN));
        }
      } else {
        // This is a one-sided range
        break;
      }
      // We have to add the value always, the key was added before
      value->toVelocyPackValue(searchValues);
      searchValues.close();
    }

    // Now handle the next element, which might be a range
    if (usedFields < _fields.size()) {
      auto it = found.find(usedFields);
      if (it != found.end()) {
        auto rangeConditions = it->second;
        TRI_ASSERT(rangeConditions.size() <= 2);
        VPackObjectBuilder searchElement(&searchValues);
        for (auto& comp : rangeConditions) {
          TRI_ASSERT(comp->numMembers() == 2);
          arangodb::aql::AstNode const* access = nullptr;
          arangodb::aql::AstNode const* value = nullptr;
          bool isReverseOrder = getValueAccess(comp, access, value);
          // Add the key
          switch (comp->type) {
            case arangodb::aql::NODE_TYPE_OPERATOR_BINARY_LT:
              if (isReverseOrder) {
                searchValues.add(VPackValue(TRI_SLICE_KEY_GT));
              } else {
                searchValues.add(VPackValue(TRI_SLICE_KEY_LT));
              }
              break;
            case arangodb::aql::NODE_TYPE_OPERATOR_BINARY_LE:
              if (isReverseOrder) {
                searchValues.add(VPackValue(TRI_SLICE_KEY_GE));
              } else {
                searchValues.add(VPackValue(TRI_SLICE_KEY_LE));
              }
              break;
            case arangodb::aql::NODE_TYPE_OPERATOR_BINARY_GT:
              if (isReverseOrder) {
                searchValues.add(VPackValue(TRI_SLICE_KEY_LT));
              } else {
                searchValues.add(VPackValue(TRI_SLICE_KEY_GT));
              }
              break;
            case arangodb::aql::NODE_TYPE_OPERATOR_BINARY_GE:
              if (isReverseOrder) {
                searchValues.add(VPackValue(TRI_SLICE_KEY_LE));
              } else {
                searchValues.add(VPackValue(TRI_SLICE_KEY_GE));
              }
              break;
          default:
            // unsupported right now. Should have been rejected by
            // supportsFilterCondition
            TRI_ASSERT(false);
            return nullptr;
          }
          value->toVelocyPackValue(searchValues);
        }
      }
    }
  }
  searchValues.close();

  TRI_IF_FAILURE("RocksDBIndex::noIterator") {
    THROW_ARANGO_EXCEPTION(TRI_ERROR_DEBUG);
  }

  if (needNormalize) {
    VPackBuilder expandedSearchValues;
    expandInSearchValues(searchValues.slice(), expandedSearchValues);
    VPackSlice expandedSlice = expandedSearchValues.slice();
    std::vector<IndexIterator*> iterators;
    try {
      for (auto const& val : VPackArrayIterator(expandedSlice)) {
        auto iterator = lookup(trx, val, reverse);
        iterators.push_back(iterator);
      }
      if (reverse) {
        std::reverse(iterators.begin(), iterators.end());
      }
    }
    catch (...) {
      for (auto& it : iterators) {
        delete it;
      }
      throw; 
    }
    return new MultiIndexIterator(iterators);
  }
  VPackSlice searchSlice = searchValues.slice();
  TRI_ASSERT(searchSlice.length() == 1);
  searchSlice = searchSlice.at(0);
  return lookup(trx, searchSlice, reverse);
}

////////////////////////////////////////////////////////////////////////////////
/// @brief specializes the condition for use with the index
////////////////////////////////////////////////////////////////////////////////

arangodb::aql::AstNode* RocksDBIndex::specializeCondition(
    arangodb::aql::AstNode* node,
    arangodb::aql::Variable const* reference) const {
  std::unordered_map<size_t, std::vector<arangodb::aql::AstNode const*>> found;
  size_t values = 0;
  matchAttributes(node, reference, found, values, false);

  std::vector<arangodb::aql::AstNode const*> children;
  bool lastContainsEquality = true;

  for (size_t i = 0; i < _fields.size(); ++i) {
    auto it = found.find(i);

    if (it == found.end()) {
      // index attribute not covered by condition
      break;
    }

    // check if the current condition contains an equality condition
    auto& nodes = (*it).second;
    bool containsEquality = false;
    for (size_t j = 0; j < nodes.size(); ++j) {
      if (nodes[j]->type == arangodb::aql::NODE_TYPE_OPERATOR_BINARY_EQ ||
          nodes[j]->type == arangodb::aql::NODE_TYPE_OPERATOR_BINARY_IN) {
        containsEquality = true;
        break;
      }
    }

    if (!lastContainsEquality) {
      // unsupported condition. must abort
      break;
    }

    std::sort(
        nodes.begin(), nodes.end(),
        [](arangodb::aql::AstNode const* lhs, arangodb::aql::AstNode const* rhs)
            -> bool { return sortWeight(lhs) < sortWeight(rhs); });

    lastContainsEquality = containsEquality;
    std::unordered_set<int> operatorsFound;
    for (auto& it : nodes) {
      // do not let duplicate or related operators pass
      if (isDuplicateOperator(it, operatorsFound)) {
        continue;
      }
      operatorsFound.emplace(static_cast<int>(it->type));
      children.emplace_back(it);
    }
  }

  while (node->numMembers() > 0) {
    node->removeMemberUnchecked(0);
  }

  for (auto& it : children) {
    node->addMember(it);
  }
  return node;
}

bool RocksDBIndex::isDuplicateOperator(
    arangodb::aql::AstNode const* node,
    std::unordered_set<int> const& operatorsFound) const {
  auto type = node->type;
  if (operatorsFound.find(static_cast<int>(type)) != operatorsFound.end()) {
    // duplicate operator
    return true;
  }

  if (operatorsFound.find(
          static_cast<int>(arangodb::aql::NODE_TYPE_OPERATOR_BINARY_EQ)) !=
          operatorsFound.end() ||
      operatorsFound.find(
          static_cast<int>(arangodb::aql::NODE_TYPE_OPERATOR_BINARY_IN)) !=
          operatorsFound.end()) {
    return true;
  }

  bool duplicate = false;
  switch (type) {
    case arangodb::aql::NODE_TYPE_OPERATOR_BINARY_LT:
      duplicate = operatorsFound.find(static_cast<int>(
                      arangodb::aql::NODE_TYPE_OPERATOR_BINARY_LE)) !=
                  operatorsFound.end();
      break;
    case arangodb::aql::NODE_TYPE_OPERATOR_BINARY_LE:
      duplicate = operatorsFound.find(static_cast<int>(
                      arangodb::aql::NODE_TYPE_OPERATOR_BINARY_LT)) !=
                  operatorsFound.end();
      break;
    case arangodb::aql::NODE_TYPE_OPERATOR_BINARY_GT:
      duplicate = operatorsFound.find(static_cast<int>(
                      arangodb::aql::NODE_TYPE_OPERATOR_BINARY_GE)) !=
                  operatorsFound.end();
      break;
    case arangodb::aql::NODE_TYPE_OPERATOR_BINARY_GE:
      duplicate = operatorsFound.find(static_cast<int>(
                      arangodb::aql::NODE_TYPE_OPERATOR_BINARY_GT)) !=
                  operatorsFound.end();
      break;
    case arangodb::aql::NODE_TYPE_OPERATOR_BINARY_EQ:
      duplicate = operatorsFound.find(static_cast<int>(
                      arangodb::aql::NODE_TYPE_OPERATOR_BINARY_IN)) !=
                  operatorsFound.end();
      break;
    case arangodb::aql::NODE_TYPE_OPERATOR_BINARY_IN:
      duplicate = operatorsFound.find(static_cast<int>(
                      arangodb::aql::NODE_TYPE_OPERATOR_BINARY_EQ)) !=
                  operatorsFound.end();
      break;
    default: {
      // ignore
    }
  }

  return duplicate;
}
