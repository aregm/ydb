# set -v
if [[ $1 != "disk" && $1 != "ram" ]]; then
  echo Please specify 'disk' or 'ram' as the parameter
  exit
fi
if [[ $1 = "disk" ]]; then
  if [ ! -f ydb.data ]; then
    echo Data file ydb.data not found, creating ...  
    fallocate -l 64G ydb.data
    if [[ $? -ge 1 ]]; then
      echo Error creating data file
      exit
    fi
  fi
  cfg=disk.yaml
else
  cfg=ram.yaml
fi
export LD_LIBRARY_PATH="$LD_LIBRARY_PATH:`pwd`/ydbd/lib"
echo Starting storage process...
`pwd`/ydbd/bin/ydbd server --yaml-config config/$cfg --node 1 --grpc-port 2136 --ic-port 19001 --mon-port 8765 --log-file-name logs/storage_start.log > logs/storage_start_output.log 2>logs/storage_start_err.log &
sleep 3
grep logs/storage_start_err.log -v -f config/exclude_err.txt
if [[ $? -eq 0 ]]; then
  echo Errors found when starting storage process, cancelling start script
  exit
fi
echo Initializing storage ...
`pwd`/ydbd/bin/ydbd -s grpc://localhost:2136 admin blobstorage config init --yaml-file config/$cfg > logs/init_storage.log 2>&1
if [[ $? -ge 1 ]]; then
  echo Errors found when initializing storage, cancelling start script, check logs/init_storage.log
  exit
fi
echo Registering database ...
`pwd`/ydbd/bin/ydbd -s grpc://localhost:2136 admin database /Root/test create ssd:1 > logs/db_reg.log 2>&1
if [[ $? -ge 1 ]]; then
  echo Errors found when registering database, cancelling start script, check logs/db_reg.log
  exit
fi
echo Starting database process...
`pwd`/ydbd/bin/ydbd server --yaml-config config/$cfg --tenant /Root/test --node-broker localhost:2136 --grpc-port 31001 --ic-port 31003 --mon-port 31002 --log-file-name logs/db_start.log > logs/db_start_output.log 2>logs/db_start_err.log &
sleep 3
grep logs/db_start_err.log -v -f config/exclude_err.txt
if [[ $? -eq 0 ]]; then
  echo Errors found when starting database process, cancelling start script
  exit
fi
echo "
Database started. Connection options for YDB CLI:

-e grpc://localhost:2136 -d /Root/test
"
