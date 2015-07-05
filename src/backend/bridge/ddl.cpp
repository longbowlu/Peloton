/*-------------------------------------------------------------------------
 *
 * dll.cpp
 * file description
 *
 * Copyright(c) 2015, CMU
 *
 * /n-store/src/bridge/ddl.cpp
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"
#include "c.h"

#include "bridge/bridge.h"
#include "nodes/parsenodes.h"
#include "parser/parse_utilcmd.h"
#include "parser/parse_type.h"
#include "access/htup_details.h"
#include "utils/resowner.h"
#include "utils/syscache.h"
#include "catalog/pg_type.h"

#include "backend/bridge/ddl.h"
#include "backend/catalog/catalog.h"
#include "backend/catalog/schema.h"
#include "backend/common/logger.h"
#include "backend/common/types.h"
#include "backend/index/index.h"
#include "backend/index/index_factory.h"
#include "backend/storage/backend_vm.h"
#include "backend/storage/table_factory.h"

#include <cassert>
#include <iostream>

namespace peloton {
namespace bridge {

/**
 * @brief Process utility statement.
 * @param parsetree Parse tree
 */
void DDL::ProcessUtility(Node *parsetree,
                         const char *queryString){
  assert(parsetree != nullptr);
  assert(queryString != nullptr);

  // Process depending on type of utility statement
  switch (nodeTag(parsetree))
  {
    case T_CreateStmt:
    case T_CreateForeignTableStmt:
    {
      List     *stmts;
      ListCell   *l;

      assert(CurrentResourceOwner != NULL);

      /* Run parse analysis ... */
      stmts = transformCreateStmt((CreateStmt *) parsetree,
                                  queryString);

      /* ... and do it */
      foreach(l, stmts)
      {
        Node     *stmt = (Node *) lfirst(l);
        if (IsA(stmt, CreateStmt))
        {
          CreateStmt* Cstmt = (CreateStmt*)stmt;
          List* schema = (List*)(Cstmt->tableElts);

          char* relation_name = Cstmt->relation->relname;
          std::vector<peloton::catalog::ColumnInfo> column_infos;

          bool status;

          // Construct DDL_ColumnInfo with schema
          if( schema != NULL ){
            column_infos = peloton::bridge::DDL::ConstructColumnInfoByParsingCreateStmt( Cstmt );
            status = peloton::bridge::DDL::CreateTable( relation_name, column_infos );
          }
          else {
            // Create Table without column info
            status = peloton::bridge::DDL::CreateTable( relation_name, column_infos );
          }
          fprintf(stderr, "DDL_CreateTable(%s) :: %d \n", Cstmt->relation->relname,status);
        }
      }
    }
    break;

    case T_IndexStmt:  /* CREATE INDEX */
    {
      IndexStmt  *stmt = (IndexStmt *) parsetree;
      Oid      relid;

      // CreateIndex
      ListCell   *entry;
      int column_itr_for_KeySchema= 0;
      int type = 0;
      bool ret;

      char**ColumnNamesForKeySchema = (char**)malloc(sizeof(char*)*stmt->indexParams->length);

      // Parse the IndexStmt and construct ddl_columnInfo for TupleSchema and KeySchema
      foreach(entry, stmt->indexParams)
      {
        IndexElem *indexElem = lfirst(entry);

        //printf("index name %s \n", indexElem->name);
        //printf("Index column %s \n", indexElem->indexcolname) ;

        if( indexElem->name != NULL )
        {
          ColumnNamesForKeySchema[column_itr_for_KeySchema] = (char*) malloc( sizeof(char*)*strlen(indexElem->name));
          strcpy(ColumnNamesForKeySchema[column_itr_for_KeySchema], indexElem->name );
          column_itr_for_KeySchema++;
        }
      }

      // look up the access method, transform the method name into IndexType
      if (strcmp(stmt->accessMethod, "btree") == 0){
        type = BTREE_AM_OID;
      }
      else if (strcmp(stmt->accessMethod, "hash") == 0){
        type = HASH_AM_OID;
      }
      else if (strcmp(stmt->accessMethod, "rtree") == 0 || strcmp(stmt->accessMethod, "gist") == 0){
        type = GIST_AM_OID;
      }
      else if (strcmp(stmt->accessMethod, "gin") == 0){
        type = GIN_AM_OID;
      }
      else if (strcmp(stmt->accessMethod, "spgist") == 0){
        type = SPGIST_AM_OID;
      }
      else if (strcmp(stmt->accessMethod, "brin") == 0){
        type = BRIN_AM_OID;
      }
      else{
        type = 0;
      }

      ret = peloton::bridge::DDL::CreateIndex(stmt->idxname,
                                              stmt->relation->relname,
                                              type,
                                              stmt->unique,
                                              ColumnNamesForKeySchema,
                                              column_itr_for_KeySchema
      );

      fprintf(stderr, "DDLCreateIndex :: %d \n", ret);

    }
    break;

    case T_DropStmt:
    {
      DropStmt* drop;
      ListCell  *cell;
      int table_oid_itr = 0;
      bool ret;
      Oid* table_oid_list;
      drop = (DropStmt*) parsetree;
      table_oid_list = (Oid*) malloc ( sizeof(Oid)*(drop->objects->length));

      foreach(cell, drop->objects)
      {
        if (drop->removeType == OBJECT_TABLE )
        {
          List* names = ((List *) lfirst(cell));
          char* table_name = strVal(linitial(names));
          table_oid_list[table_oid_itr++] = GetRelationOid(table_name);
        }
      }

      while(table_oid_itr > 0)
      {
        ret  = peloton::bridge::DDL::DropTable(table_oid_list[--table_oid_itr]);
        fprintf(stderr, "DDLDropTable :: %d \n", ret);
      }

    }
    break;

    default:
      elog(LOG, "unrecognized node type: %d",
           (int) nodeTag(parsetree));
      break;
  }

}

/**
 * @brief Construct ColumnInfo vector from a create statement
 * @param Cstmt a create statement 
 * @return ColumnInfo vector 
 */
std::vector<catalog::ColumnInfo> DDL::ConstructColumnInfoByParsingCreateStmt( CreateStmt* Cstmt ){
  assert(Cstmt);

  // Get the column list from the create statement
  List* ColumnList = (List*)(Cstmt->tableElts);
  std::vector<catalog::ColumnInfo> column_infos;

  //===--------------------------------------------------------------------===//
  // Column Type Information
  //===--------------------------------------------------------------------===//

  // Parse the CreateStmt and construct ColumnInfo
  ListCell   *entry;
  foreach(entry, ColumnList){

    ColumnDef  *coldef = lfirst(entry);

    // Parsing the column value type
    Oid typeoid;
    int32 typemod;
    int typelen;

    // Get the type oid and type mod with given typeName
    typeoid = typenameTypeId(NULL, coldef->typeName);

    typenameTypeIdAndMod(NULL, coldef->typeName, &typeoid, &typemod);

    // Get type length
    Type tup = typeidType(typeoid);
    typelen = typeLen(tup);
    ReleaseSysCache(tup);

    // For a fixed-size type, typlen is the number of bytes in the internal
    // representation of the type. But for a variable-length type, typlen is negative.
    if( typelen == -1 )
    {
      printf("%u\n", typemod);
      // we need to get the atttypmod from pg_attribute
    }
     

    ValueType column_valueType = PostgresValueTypeToPelotonValueType( (PostgresValueType) typeoid );
    int column_length = typelen;
    std::string column_name = coldef->colname;
    bool column_allow_null = !coldef->is_not_null;

    //===--------------------------------------------------------------------===//
    // Column Constraint Information
    //===--------------------------------------------------------------------===//
    std::vector<catalog::Constraint> column_constraints;

    if( coldef->constraints != NULL){
      ListCell* constNodeEntry;

      foreach(constNodeEntry, coldef->constraints)
      {
        Constraint* ConstraintNode = lfirst(constNodeEntry);
        ConstraintType contype;
        std::string conname;

        // Get constraint type
        contype = PostgresConstraintTypeToPelotonConstraintType( (PostgresConstraintType) ConstraintNode->contype );
        std::cout << ConstraintTypeToString(contype) << std::endl;

        // Get constraint name
        if( ConstraintNode->conname != NULL)
          conname = ConstraintNode->conname;

        catalog::Constraint* constraint = new catalog::Constraint( contype, conname );
        column_constraints.push_back(*constraint);
      }
    }// end of parsing constraint 

    catalog::ColumnInfo *column_info = new catalog::ColumnInfo( column_valueType, 
                                                                column_length, 
                                                                column_name, 
                                                                column_constraints);

    // Insert column_info into ColumnInfos
    column_infos.push_back(*column_info);
  }// end of parsing column list

  return column_infos;
}

/**
 * @brief Create table.
 * @param table_name Table name
 * @param column_infos Information about the columns
 * @param schema Schema for the table
 * @return true if we created a table, false otherwise
 */
bool DDL::CreateTable( std::string table_name,
                        std::vector<catalog::ColumnInfo> column_infos,
                        catalog::Schema *schema){

  //===--------------------------------------------------------------------===//
  // Check Parameters 
  //===--------------------------------------------------------------------===//
  assert( !table_name.empty() );
  assert( column_infos.size() > 0 || schema != NULL );

  Oid database_oid = GetCurrentDatabaseOid();
  if(database_oid == InvalidOid)
    return false;

  // Construct our schema from vector of ColumnInfo
  if( schema == NULL) 
    schema = new catalog::Schema(column_infos);

  // FIXME: Construct table backend
  storage::VMBackend *backend = new storage::VMBackend();

  // Build a table from schema
  storage::DataTable *table = storage::TableFactory::GetDataTable(database_oid, schema, table_name);

  catalog::Schema* our_schema = table->GetSchema();

  
  std::cout << "Print out created table schema for debugginh " << std::endl << *our_schema << std::endl;


  if(table != nullptr) {
    LOG_INFO("Created table : %s", table_name.c_str());
    return true;
  }

  return false;
}


/**
 * @brief Drop table.
 * @param table_oid Table id.
 * @return true if we dropped the table, false otherwise
 */
bool DDL::DropTable(Oid table_oid) {

  oid_t database_oid = GetCurrentDatabaseOid();

  if(database_oid == InvalidOid || table_oid == InvalidOid) {
    LOG_WARN("Could not drop table :: db oid : %u table oid : %u", database_oid, table_oid);
    return false;
  }

  bool status = storage::TableFactory::DropDataTable(database_oid, table_oid);
  if(status == true) {
    LOG_INFO("Dropped table with oid : %u\n", table_oid);
    return true;
  }

  return false;
}


/**
 * @brief Create index.
 * @param index_name Index name
 * @param table_name Table name
 * @param type Type of the index
 * @param unique Index is unique or not ?
 * @param
 * @param key_column_name key column names
 * @param num_columns Number of columns in the table
 * @return true if we create the index, false otherwise
 */
bool DDL::CreateIndex(std::string index_name,
                      std::string table_name,
                      int index_type,
                      bool unique_keys,
                      char **key_column_names,
                      int num_columns_in_key) {

  assert( index_name != "" || table_name != "" || key_column_names != NULL || num_columns_in_key > 0 );

  // NOTE: We currently only support btree as our index implementation
  // FIXME: Support other types based on "type" argument
  IndexMethodType our_index_type = INDEX_METHOD_TYPE_BTREE_MULTIMAP;

  // Get the database oid and table oid
  oid_t database_oid = GetCurrentDatabaseOid();
  oid_t table_oid = GetRelationOid(table_name.c_str());

  // Get the table location from manager
  auto table = catalog::Manager::GetInstance().GetLocation(database_oid, table_oid);
  storage::DataTable* data_table = (storage::DataTable*) table;
  auto tuple_schema = data_table->GetSchema();

  // Construct key schema
  std::vector<oid_t> key_columns;

  // Based on the key column info, get the oid of the given 'key' columns in the tuple schema
  for(oid_t key_schema_column_itr = 0;  key_schema_column_itr < num_columns_in_key; key_schema_column_itr++) {
    for( oid_t tuple_schema_column_itr = 0; tuple_schema_column_itr < tuple_schema->GetColumnCount();
        tuple_schema_column_itr++){

      // Get the current column info from tuple schema
      catalog::ColumnInfo column_info = tuple_schema->GetColumnInfo(tuple_schema_column_itr);

      // Compare Key Schema's current column name and Tuple Schema's current column name
      if(strcmp(key_column_names[key_schema_column_itr], (column_info.name).c_str() )== 0 )
        key_columns.push_back(tuple_schema_column_itr);
    }
  }

  auto key_schema = catalog::Schema::CopySchema(tuple_schema, key_columns);

  // Create index metadata and physical index
  index::IndexMetadata* metadata = new index::IndexMetadata(index_name, our_index_type,
                                                            tuple_schema, key_schema,
                                                            unique_keys);
  index::Index* index = index::IndexFactory::GetInstance(metadata);

  // Record the built index in the table
  data_table->AddIndex(index);

  return true;
}

/**
 * @brief Create index.
 * @param index_name Index name
 * @param table_name Table name
 * @param type Type of the index
 * @param unique Index is unique or not ?
 * @param key_column_names column names for the key table 
 * @return true if we create the index, false otherwise
 */
bool DDL::CreateIndex2(std::string index_name,
                       std::string table_name,
                       IndexMethodType  index_method_type,
                       IndexType  index_type,
                       bool unique_keys,
                       std::vector<std::string> key_column_names,
                       bool bootstrap ){

  assert( index_name != "" );
  assert( table_name != "" );
  assert( key_column_names.size() > 0  );

  // NOTE: We currently only support btree as our index implementation
  // TODO : Support other types based on "type" argument
  IndexMethodType our_index_type = INDEX_METHOD_TYPE_BTREE_MULTIMAP;

  // Get the database oid and table oid
  oid_t database_oid = GetCurrentDatabaseOid();
  oid_t table_oid = GetRelationOid(table_name.c_str());

  // Get the table location from manager
  auto table = catalog::Manager::GetInstance().GetLocation(database_oid, table_oid);
  storage::DataTable* data_table = (storage::DataTable*) table;
  auto tuple_schema = data_table->GetSchema();

  // Construct key schema
  std::vector<oid_t> key_columns;

  // Based on the key column info, get the oid of the given 'key' columns in the tuple schema
  for( auto key_column_name : key_column_names ){
    for( oid_t tuple_schema_column_itr = 0; tuple_schema_column_itr < tuple_schema->GetColumnCount();
        tuple_schema_column_itr++){

      // Get the current column info from tuple schema
      catalog::ColumnInfo column_info = tuple_schema->GetColumnInfo(tuple_schema_column_itr);
      // Compare Key Schema's current column name and Tuple Schema's current column name
      if( key_column_name == column_info.name ){
        key_columns.push_back(tuple_schema_column_itr);

        // TODO :: Need to talk with Joy
        // NOTE :: Since pg_attribute doesn't have any information about primary key and unique key,
        //         I try to store these information when we create an unique and primary key index
        if( bootstrap ){
          if( index_type == INDEX_TYPE_PRIMARY_KEY ){ 
            catalog::Constraint* constraint = new catalog::Constraint( CONSTRAINT_TYPE_PRIMARY );
            tuple_schema->AddConstraintInColumn( tuple_schema_column_itr, constraint); 
          }else if( index_type == INDEX_TYPE_UNIQUE ){ 
            catalog::Constraint* constraint = new catalog::Constraint( CONSTRAINT_TYPE_UNIQUE );
            tuple_schema->AddConstraintInColumn( tuple_schema_column_itr, constraint); 
          }
        }

      }
    }
  }

  auto key_schema = catalog::Schema::CopySchema(tuple_schema, key_columns);

  // Create index metadata and physical index
  index::IndexMetadata* metadata = new index::IndexMetadata(index_name, our_index_type,
                                                            tuple_schema, key_schema,
                                                            unique_keys);
  index::Index* index = index::IndexFactory::GetInstance(metadata);

  // Record the built index in the table
  switch(  index_type ){
  case INDEX_TYPE_NORMAL:
	  data_table->AddIndex(index);
	  break;
  case INDEX_TYPE_PRIMARY_KEY:
	  data_table->SetPrimaryIndex(index);
	  break;
  case INDEX_TYPE_UNIQUE:
	  data_table->AddUniqueIndex(index);
	  break;
  default:
      elog(LOG, "unrecognized index type: %d", index_type);
  }

  return true;
}

} // namespace bridge
} // namespace peloton
