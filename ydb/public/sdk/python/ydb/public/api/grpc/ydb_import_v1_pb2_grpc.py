# Generated by the gRPC Python protocol compiler plugin. DO NOT EDIT!
"""Client and server classes corresponding to protobuf-defined services."""
import grpc

from ydb.public.api.protos import ydb_import_pb2 as ydb_dot_public_dot_api_dot_protos_dot_ydb__import__pb2


class ImportServiceStub(object):
    """Missing associated documentation comment in .proto file."""

    def __init__(self, channel):
        """Constructor.

        Args:
            channel: A grpc.Channel.
        """
        self.ImportFromS3 = channel.unary_unary(
                '/Ydb.Import.V1.ImportService/ImportFromS3',
                request_serializer=ydb_dot_public_dot_api_dot_protos_dot_ydb__import__pb2.ImportFromS3Request.SerializeToString,
                response_deserializer=ydb_dot_public_dot_api_dot_protos_dot_ydb__import__pb2.ImportFromS3Response.FromString,
                )
        self.ImportData = channel.unary_unary(
                '/Ydb.Import.V1.ImportService/ImportData',
                request_serializer=ydb_dot_public_dot_api_dot_protos_dot_ydb__import__pb2.ImportDataRequest.SerializeToString,
                response_deserializer=ydb_dot_public_dot_api_dot_protos_dot_ydb__import__pb2.ImportDataResponse.FromString,
                )


class ImportServiceServicer(object):
    """Missing associated documentation comment in .proto file."""

    def ImportFromS3(self, request, context):
        """Imports data from S3.
        Method starts an asynchronous operation that can be cancelled while it is in progress.
        """
        context.set_code(grpc.StatusCode.UNIMPLEMENTED)
        context.set_details('Method not implemented!')
        raise NotImplementedError('Method not implemented!')

    def ImportData(self, request, context):
        """Writes data to a table.
        Method accepts serialized data in the selected format and writes it non-transactionally.
        """
        context.set_code(grpc.StatusCode.UNIMPLEMENTED)
        context.set_details('Method not implemented!')
        raise NotImplementedError('Method not implemented!')


def add_ImportServiceServicer_to_server(servicer, server):
    rpc_method_handlers = {
            'ImportFromS3': grpc.unary_unary_rpc_method_handler(
                    servicer.ImportFromS3,
                    request_deserializer=ydb_dot_public_dot_api_dot_protos_dot_ydb__import__pb2.ImportFromS3Request.FromString,
                    response_serializer=ydb_dot_public_dot_api_dot_protos_dot_ydb__import__pb2.ImportFromS3Response.SerializeToString,
            ),
            'ImportData': grpc.unary_unary_rpc_method_handler(
                    servicer.ImportData,
                    request_deserializer=ydb_dot_public_dot_api_dot_protos_dot_ydb__import__pb2.ImportDataRequest.FromString,
                    response_serializer=ydb_dot_public_dot_api_dot_protos_dot_ydb__import__pb2.ImportDataResponse.SerializeToString,
            ),
    }
    generic_handler = grpc.method_handlers_generic_handler(
            'Ydb.Import.V1.ImportService', rpc_method_handlers)
    server.add_generic_rpc_handlers((generic_handler,))


 # This class is part of an EXPERIMENTAL API.
class ImportService(object):
    """Missing associated documentation comment in .proto file."""

    @staticmethod
    def ImportFromS3(request,
            target,
            options=(),
            channel_credentials=None,
            call_credentials=None,
            insecure=False,
            compression=None,
            wait_for_ready=None,
            timeout=None,
            metadata=None):
        return grpc.experimental.unary_unary(request, target, '/Ydb.Import.V1.ImportService/ImportFromS3',
            ydb_dot_public_dot_api_dot_protos_dot_ydb__import__pb2.ImportFromS3Request.SerializeToString,
            ydb_dot_public_dot_api_dot_protos_dot_ydb__import__pb2.ImportFromS3Response.FromString,
            options, channel_credentials,
            insecure, call_credentials, compression, wait_for_ready, timeout, metadata)

    @staticmethod
    def ImportData(request,
            target,
            options=(),
            channel_credentials=None,
            call_credentials=None,
            insecure=False,
            compression=None,
            wait_for_ready=None,
            timeout=None,
            metadata=None):
        return grpc.experimental.unary_unary(request, target, '/Ydb.Import.V1.ImportService/ImportData',
            ydb_dot_public_dot_api_dot_protos_dot_ydb__import__pb2.ImportDataRequest.SerializeToString,
            ydb_dot_public_dot_api_dot_protos_dot_ydb__import__pb2.ImportDataResponse.FromString,
            options, channel_credentials,
            insecure, call_credentials, compression, wait_for_ready, timeout, metadata)
