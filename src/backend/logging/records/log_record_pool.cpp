#include "log_record_pool.h"

namespace peloton {
namespace logging {

void LogRecordList::Clear() {
  // Clean up
  while (head_node != nullptr) {
    LogRecordNode *deletingNode = head_node;
    head_node = head_node->next_node;
    _backend->Sync(this);
    // XXX cannot ensure free success if _backend is newly created while the
    // node is created by last execution...
    _backend->Free(deletingNode);
  }
  tail_node = nullptr;
}

int LogRecordList::AddLogRecord(TupleRecord *record) {
  if (_backend == nullptr || record == nullptr || record->GetTransactionId() != txn_id)
    return -1;

  LogRecordNode *localRecord = (LogRecordNode*) _backend->Allocate(sizeof(LogRecordNode));
  if (localRecord != nullptr) {
    localRecord->_db_oid = record->GetDatabaseOid();
    localRecord->_delete_location = record->GetDeleteLocation();
    localRecord->_insert_location = record->GetInsertLocation();
    localRecord->_log_record_type = record->GetType();
    localRecord->_table_oid = record->GetTableId();
    localRecord->next_node = nullptr;

    _backend->Sync(localRecord);

    // add to the list
    if (tail_node == nullptr) {
      head_node = localRecord;
      _backend->Sync(this);
    } else {
      tail_node->next_node = localRecord;
      _backend->Sync(tail_node);
    }
    tail_node = localRecord;
    return 0;
  } else {
    return -1;
  }
}

void LogRecordList::CheckLogRecordList(storage::AbstractBackend *backend) {
  assert(backend != nullptr);
  _backend = backend;
  if (head_node == nullptr) {
    tail_node = nullptr;
    return;
  }
  LogRecordNode * cur = head_node;
  while(cur->next_node != nullptr) {
    cur = cur->next_node;
  }
  if (tail_node != cur) {
    tail_node = cur;
  }
}

void LogRecordPool::Clear() {
  // Clean up
  if (txn_log_table != nullptr) {
    txn_log_table->clear();
    delete txn_log_table;
    txn_log_table = nullptr;
  }

  while (head_list != nullptr) {
    RemoveLogList(head_list);
  }
  tail_list = nullptr;
}

int LogRecordPool::CreateTxnLogList(txn_id_t txn_id) {
  if (_backend == nullptr) {
    return -1;
  }
  LogRecordList* existing_list = SearchRecordList(txn_id);
  if (existing_list == nullptr) {
    existing_list = (LogRecordList*) _backend->Allocate(sizeof(LogRecordList));
    if (existing_list == nullptr) {
      return -1;
    }
    existing_list->init(_backend, txn_id);
    // ensure new node is persisted
    _backend->Sync(existing_list);
    txn_log_table->insert(std::pair<txn_id_t, LogRecordList*>(txn_id, existing_list));
    // add to the pool
    if (tail_list == nullptr) {
      head_list = existing_list;
      // ensure new node info is linked in the list
      _backend->Sync(this);
      existing_list->SetPrevList(nullptr);
    } else {
      tail_list->SetNextList(existing_list);
      existing_list->SetPrevList(tail_list);
    }
    tail_list = existing_list;
  }
  return 0;
}

int LogRecordPool::AddLogRecord(TupleRecord *record) {
  LogRecordList* existing_list = SearchRecordList(record->GetTransactionId());
  if (existing_list != nullptr) {
    return existing_list->AddLogRecord(record);
  }
  return -1;
}

void LogRecordPool::RemoveTxnLogList(txn_id_t txn_id) {
  LogRecordList* removing_list = SearchRecordList(txn_id);
  if (removing_list != nullptr) {
    RemoveLogList(removing_list);
    txn_log_table->erase(txn_id);
  }
}

LogRecordList* LogRecordPool::SearchRecordList(txn_id_t txn_id) const {
  if (txn_log_table->find(txn_id) != txn_log_table->end()) {
    return txn_log_table->at(txn_id);
  } else {
    return nullptr;
  }
}

void LogRecordPool::RemoveLogList(LogRecordList *list) {
  if (list->GetPrevList() == nullptr) {
    head_list = list->GetNextList();
    _backend->Sync(this);
  } else {
    list->GetPrevList()->SetNextList(list->GetNextList());
  }
  if (list->GetNextList() == nullptr) {
    tail_list = list->GetPrevList();
  } else {
    list->GetNextList()->SetPrevList(list->GetPrevList());
  }
  // clean the list record
  list->Clear();
  _backend->Free(list);
}

void LogRecordPool::CheckLogRecordPool(storage::AbstractBackend *backend) {
  assert(backend != nullptr);
  _backend = backend;

  txn_log_table = new std::map<txn_id_t, LogRecordList *>();

  if (head_list == nullptr) {
    tail_list = nullptr;
    return;
  }
  LogRecordList * prev = nullptr;
  LogRecordList * cur = head_list;
  while(cur->GetNextList() != nullptr) {
    txn_log_table->insert(std::pair<txn_id_t, LogRecordList*>(cur->GetTxnID(), cur));
    if (cur->GetPrevList() != prev) {
      cur->SetPrevList(prev);
    }
    prev = cur;
    cur->CheckLogRecordList(backend);
    cur = cur->GetNextList();
  }
  txn_log_table->insert(std::pair<txn_id_t, LogRecordList*>(cur->GetTxnID(), cur));
  tail_list = cur;
}

}
}
