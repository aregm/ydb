# -*- coding: utf-8 -*-
# Generated by the protocol buffer compiler.  DO NOT EDIT!
# source: ydb/public/api/grpc/ydb_table_v1.proto
"""Generated protocol buffer code."""
from google.protobuf import descriptor as _descriptor
from google.protobuf import message as _message
from google.protobuf import reflection as _reflection
from google.protobuf import symbol_database as _symbol_database
# @@protoc_insertion_point(imports)

_sym_db = _symbol_database.Default()


from ydb.public.api.protos import ydb_table_pb2 as ydb_dot_public_dot_api_dot_protos_dot_ydb__table__pb2


DESCRIPTOR = _descriptor.FileDescriptor(
  name='ydb/public/api/grpc/ydb_table_v1.proto',
  package='Ydb.Table.V1',
  syntax='proto3',
  serialized_options=b'\n\027com.yandex.ydb.table.v1',
  create_key=_descriptor._internal_create_key,
  serialized_pb=b'\n&ydb/public/api/grpc/ydb_table_v1.proto\x12\x0cYdb.Table.V1\x1a%ydb/public/api/protos/ydb_table.proto2\xa4\x0e\n\x0cTableService\x12R\n\rCreateSession\x12\x1f.Ydb.Table.CreateSessionRequest\x1a .Ydb.Table.CreateSessionResponse\x12R\n\rDeleteSession\x12\x1f.Ydb.Table.DeleteSessionRequest\x1a .Ydb.Table.DeleteSessionResponse\x12\x46\n\tKeepAlive\x12\x1b.Ydb.Table.KeepAliveRequest\x1a\x1c.Ydb.Table.KeepAliveResponse\x12L\n\x0b\x43reateTable\x12\x1d.Ydb.Table.CreateTableRequest\x1a\x1e.Ydb.Table.CreateTableResponse\x12\x46\n\tDropTable\x12\x1b.Ydb.Table.DropTableRequest\x1a\x1c.Ydb.Table.DropTableResponse\x12I\n\nAlterTable\x12\x1c.Ydb.Table.AlterTableRequest\x1a\x1d.Ydb.Table.AlterTableResponse\x12\x46\n\tCopyTable\x12\x1b.Ydb.Table.CopyTableRequest\x1a\x1c.Ydb.Table.CopyTableResponse\x12I\n\nCopyTables\x12\x1c.Ydb.Table.CopyTablesRequest\x1a\x1d.Ydb.Table.CopyTablesResponse\x12O\n\x0cRenameTables\x12\x1e.Ydb.Table.RenameTablesRequest\x1a\x1f.Ydb.Table.RenameTablesResponse\x12R\n\rDescribeTable\x12\x1f.Ydb.Table.DescribeTableRequest\x1a .Ydb.Table.DescribeTableResponse\x12[\n\x10\x45xplainDataQuery\x12\".Ydb.Table.ExplainDataQueryRequest\x1a#.Ydb.Table.ExplainDataQueryResponse\x12[\n\x10PrepareDataQuery\x12\".Ydb.Table.PrepareDataQueryRequest\x1a#.Ydb.Table.PrepareDataQueryResponse\x12[\n\x10\x45xecuteDataQuery\x12\".Ydb.Table.ExecuteDataQueryRequest\x1a#.Ydb.Table.ExecuteDataQueryResponse\x12\x61\n\x12\x45xecuteSchemeQuery\x12$.Ydb.Table.ExecuteSchemeQueryRequest\x1a%.Ydb.Table.ExecuteSchemeQueryResponse\x12[\n\x10\x42\x65ginTransaction\x12\".Ydb.Table.BeginTransactionRequest\x1a#.Ydb.Table.BeginTransactionResponse\x12^\n\x11\x43ommitTransaction\x12#.Ydb.Table.CommitTransactionRequest\x1a$.Ydb.Table.CommitTransactionResponse\x12\x64\n\x13RollbackTransaction\x12%.Ydb.Table.RollbackTransactionRequest\x1a&.Ydb.Table.RollbackTransactionResponse\x12g\n\x14\x44\x65scribeTableOptions\x12&.Ydb.Table.DescribeTableOptionsRequest\x1a\'.Ydb.Table.DescribeTableOptionsResponse\x12N\n\x0fStreamReadTable\x12\x1b.Ydb.Table.ReadTableRequest\x1a\x1c.Ydb.Table.ReadTableResponse0\x01\x12I\n\nBulkUpsert\x12\x1c.Ydb.Table.BulkUpsertRequest\x1a\x1d.Ydb.Table.BulkUpsertResponse\x12j\n\x16StreamExecuteScanQuery\x12\".Ydb.Table.ExecuteScanQueryRequest\x1a*.Ydb.Table.ExecuteScanQueryPartialResponse0\x01\x42\x19\n\x17\x63om.yandex.ydb.table.v1b\x06proto3'
  ,
  dependencies=[ydb_dot_public_dot_api_dot_protos_dot_ydb__table__pb2.DESCRIPTOR,])



_sym_db.RegisterFileDescriptor(DESCRIPTOR)


DESCRIPTOR._options = None

_TABLESERVICE = _descriptor.ServiceDescriptor(
  name='TableService',
  full_name='Ydb.Table.V1.TableService',
  file=DESCRIPTOR,
  index=0,
  serialized_options=None,
  create_key=_descriptor._internal_create_key,
  serialized_start=96,
  serialized_end=1924,
  methods=[
  _descriptor.MethodDescriptor(
    name='CreateSession',
    full_name='Ydb.Table.V1.TableService.CreateSession',
    index=0,
    containing_service=None,
    input_type=ydb_dot_public_dot_api_dot_protos_dot_ydb__table__pb2._CREATESESSIONREQUEST,
    output_type=ydb_dot_public_dot_api_dot_protos_dot_ydb__table__pb2._CREATESESSIONRESPONSE,
    serialized_options=None,
    create_key=_descriptor._internal_create_key,
  ),
  _descriptor.MethodDescriptor(
    name='DeleteSession',
    full_name='Ydb.Table.V1.TableService.DeleteSession',
    index=1,
    containing_service=None,
    input_type=ydb_dot_public_dot_api_dot_protos_dot_ydb__table__pb2._DELETESESSIONREQUEST,
    output_type=ydb_dot_public_dot_api_dot_protos_dot_ydb__table__pb2._DELETESESSIONRESPONSE,
    serialized_options=None,
    create_key=_descriptor._internal_create_key,
  ),
  _descriptor.MethodDescriptor(
    name='KeepAlive',
    full_name='Ydb.Table.V1.TableService.KeepAlive',
    index=2,
    containing_service=None,
    input_type=ydb_dot_public_dot_api_dot_protos_dot_ydb__table__pb2._KEEPALIVEREQUEST,
    output_type=ydb_dot_public_dot_api_dot_protos_dot_ydb__table__pb2._KEEPALIVERESPONSE,
    serialized_options=None,
    create_key=_descriptor._internal_create_key,
  ),
  _descriptor.MethodDescriptor(
    name='CreateTable',
    full_name='Ydb.Table.V1.TableService.CreateTable',
    index=3,
    containing_service=None,
    input_type=ydb_dot_public_dot_api_dot_protos_dot_ydb__table__pb2._CREATETABLEREQUEST,
    output_type=ydb_dot_public_dot_api_dot_protos_dot_ydb__table__pb2._CREATETABLERESPONSE,
    serialized_options=None,
    create_key=_descriptor._internal_create_key,
  ),
  _descriptor.MethodDescriptor(
    name='DropTable',
    full_name='Ydb.Table.V1.TableService.DropTable',
    index=4,
    containing_service=None,
    input_type=ydb_dot_public_dot_api_dot_protos_dot_ydb__table__pb2._DROPTABLEREQUEST,
    output_type=ydb_dot_public_dot_api_dot_protos_dot_ydb__table__pb2._DROPTABLERESPONSE,
    serialized_options=None,
    create_key=_descriptor._internal_create_key,
  ),
  _descriptor.MethodDescriptor(
    name='AlterTable',
    full_name='Ydb.Table.V1.TableService.AlterTable',
    index=5,
    containing_service=None,
    input_type=ydb_dot_public_dot_api_dot_protos_dot_ydb__table__pb2._ALTERTABLEREQUEST,
    output_type=ydb_dot_public_dot_api_dot_protos_dot_ydb__table__pb2._ALTERTABLERESPONSE,
    serialized_options=None,
    create_key=_descriptor._internal_create_key,
  ),
  _descriptor.MethodDescriptor(
    name='CopyTable',
    full_name='Ydb.Table.V1.TableService.CopyTable',
    index=6,
    containing_service=None,
    input_type=ydb_dot_public_dot_api_dot_protos_dot_ydb__table__pb2._COPYTABLEREQUEST,
    output_type=ydb_dot_public_dot_api_dot_protos_dot_ydb__table__pb2._COPYTABLERESPONSE,
    serialized_options=None,
    create_key=_descriptor._internal_create_key,
  ),
  _descriptor.MethodDescriptor(
    name='CopyTables',
    full_name='Ydb.Table.V1.TableService.CopyTables',
    index=7,
    containing_service=None,
    input_type=ydb_dot_public_dot_api_dot_protos_dot_ydb__table__pb2._COPYTABLESREQUEST,
    output_type=ydb_dot_public_dot_api_dot_protos_dot_ydb__table__pb2._COPYTABLESRESPONSE,
    serialized_options=None,
    create_key=_descriptor._internal_create_key,
  ),
  _descriptor.MethodDescriptor(
    name='RenameTables',
    full_name='Ydb.Table.V1.TableService.RenameTables',
    index=8,
    containing_service=None,
    input_type=ydb_dot_public_dot_api_dot_protos_dot_ydb__table__pb2._RENAMETABLESREQUEST,
    output_type=ydb_dot_public_dot_api_dot_protos_dot_ydb__table__pb2._RENAMETABLESRESPONSE,
    serialized_options=None,
    create_key=_descriptor._internal_create_key,
  ),
  _descriptor.MethodDescriptor(
    name='DescribeTable',
    full_name='Ydb.Table.V1.TableService.DescribeTable',
    index=9,
    containing_service=None,
    input_type=ydb_dot_public_dot_api_dot_protos_dot_ydb__table__pb2._DESCRIBETABLEREQUEST,
    output_type=ydb_dot_public_dot_api_dot_protos_dot_ydb__table__pb2._DESCRIBETABLERESPONSE,
    serialized_options=None,
    create_key=_descriptor._internal_create_key,
  ),
  _descriptor.MethodDescriptor(
    name='ExplainDataQuery',
    full_name='Ydb.Table.V1.TableService.ExplainDataQuery',
    index=10,
    containing_service=None,
    input_type=ydb_dot_public_dot_api_dot_protos_dot_ydb__table__pb2._EXPLAINDATAQUERYREQUEST,
    output_type=ydb_dot_public_dot_api_dot_protos_dot_ydb__table__pb2._EXPLAINDATAQUERYRESPONSE,
    serialized_options=None,
    create_key=_descriptor._internal_create_key,
  ),
  _descriptor.MethodDescriptor(
    name='PrepareDataQuery',
    full_name='Ydb.Table.V1.TableService.PrepareDataQuery',
    index=11,
    containing_service=None,
    input_type=ydb_dot_public_dot_api_dot_protos_dot_ydb__table__pb2._PREPAREDATAQUERYREQUEST,
    output_type=ydb_dot_public_dot_api_dot_protos_dot_ydb__table__pb2._PREPAREDATAQUERYRESPONSE,
    serialized_options=None,
    create_key=_descriptor._internal_create_key,
  ),
  _descriptor.MethodDescriptor(
    name='ExecuteDataQuery',
    full_name='Ydb.Table.V1.TableService.ExecuteDataQuery',
    index=12,
    containing_service=None,
    input_type=ydb_dot_public_dot_api_dot_protos_dot_ydb__table__pb2._EXECUTEDATAQUERYREQUEST,
    output_type=ydb_dot_public_dot_api_dot_protos_dot_ydb__table__pb2._EXECUTEDATAQUERYRESPONSE,
    serialized_options=None,
    create_key=_descriptor._internal_create_key,
  ),
  _descriptor.MethodDescriptor(
    name='ExecuteSchemeQuery',
    full_name='Ydb.Table.V1.TableService.ExecuteSchemeQuery',
    index=13,
    containing_service=None,
    input_type=ydb_dot_public_dot_api_dot_protos_dot_ydb__table__pb2._EXECUTESCHEMEQUERYREQUEST,
    output_type=ydb_dot_public_dot_api_dot_protos_dot_ydb__table__pb2._EXECUTESCHEMEQUERYRESPONSE,
    serialized_options=None,
    create_key=_descriptor._internal_create_key,
  ),
  _descriptor.MethodDescriptor(
    name='BeginTransaction',
    full_name='Ydb.Table.V1.TableService.BeginTransaction',
    index=14,
    containing_service=None,
    input_type=ydb_dot_public_dot_api_dot_protos_dot_ydb__table__pb2._BEGINTRANSACTIONREQUEST,
    output_type=ydb_dot_public_dot_api_dot_protos_dot_ydb__table__pb2._BEGINTRANSACTIONRESPONSE,
    serialized_options=None,
    create_key=_descriptor._internal_create_key,
  ),
  _descriptor.MethodDescriptor(
    name='CommitTransaction',
    full_name='Ydb.Table.V1.TableService.CommitTransaction',
    index=15,
    containing_service=None,
    input_type=ydb_dot_public_dot_api_dot_protos_dot_ydb__table__pb2._COMMITTRANSACTIONREQUEST,
    output_type=ydb_dot_public_dot_api_dot_protos_dot_ydb__table__pb2._COMMITTRANSACTIONRESPONSE,
    serialized_options=None,
    create_key=_descriptor._internal_create_key,
  ),
  _descriptor.MethodDescriptor(
    name='RollbackTransaction',
    full_name='Ydb.Table.V1.TableService.RollbackTransaction',
    index=16,
    containing_service=None,
    input_type=ydb_dot_public_dot_api_dot_protos_dot_ydb__table__pb2._ROLLBACKTRANSACTIONREQUEST,
    output_type=ydb_dot_public_dot_api_dot_protos_dot_ydb__table__pb2._ROLLBACKTRANSACTIONRESPONSE,
    serialized_options=None,
    create_key=_descriptor._internal_create_key,
  ),
  _descriptor.MethodDescriptor(
    name='DescribeTableOptions',
    full_name='Ydb.Table.V1.TableService.DescribeTableOptions',
    index=17,
    containing_service=None,
    input_type=ydb_dot_public_dot_api_dot_protos_dot_ydb__table__pb2._DESCRIBETABLEOPTIONSREQUEST,
    output_type=ydb_dot_public_dot_api_dot_protos_dot_ydb__table__pb2._DESCRIBETABLEOPTIONSRESPONSE,
    serialized_options=None,
    create_key=_descriptor._internal_create_key,
  ),
  _descriptor.MethodDescriptor(
    name='StreamReadTable',
    full_name='Ydb.Table.V1.TableService.StreamReadTable',
    index=18,
    containing_service=None,
    input_type=ydb_dot_public_dot_api_dot_protos_dot_ydb__table__pb2._READTABLEREQUEST,
    output_type=ydb_dot_public_dot_api_dot_protos_dot_ydb__table__pb2._READTABLERESPONSE,
    serialized_options=None,
    create_key=_descriptor._internal_create_key,
  ),
  _descriptor.MethodDescriptor(
    name='BulkUpsert',
    full_name='Ydb.Table.V1.TableService.BulkUpsert',
    index=19,
    containing_service=None,
    input_type=ydb_dot_public_dot_api_dot_protos_dot_ydb__table__pb2._BULKUPSERTREQUEST,
    output_type=ydb_dot_public_dot_api_dot_protos_dot_ydb__table__pb2._BULKUPSERTRESPONSE,
    serialized_options=None,
    create_key=_descriptor._internal_create_key,
  ),
  _descriptor.MethodDescriptor(
    name='StreamExecuteScanQuery',
    full_name='Ydb.Table.V1.TableService.StreamExecuteScanQuery',
    index=20,
    containing_service=None,
    input_type=ydb_dot_public_dot_api_dot_protos_dot_ydb__table__pb2._EXECUTESCANQUERYREQUEST,
    output_type=ydb_dot_public_dot_api_dot_protos_dot_ydb__table__pb2._EXECUTESCANQUERYPARTIALRESPONSE,
    serialized_options=None,
    create_key=_descriptor._internal_create_key,
  ),
])
_sym_db.RegisterServiceDescriptor(_TABLESERVICE)

DESCRIPTOR.services_by_name['TableService'] = _TABLESERVICE

# @@protoc_insertion_point(module_scope)
