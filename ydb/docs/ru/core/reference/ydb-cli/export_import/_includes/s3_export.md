# Выгрузка в S3-совместимое хранилище

Команда `export s3` стартует на стороне сервера процесс выгрузки в S3-совместимое хранилище данных и информации об объектах схемы данных, в описанном в статье [Файловая структура](../file_structure.md) формате:

```bash
{{ ydb-cli }} [connection options] export s3 [options]
```

{% include [conn_options_ref.md](../../commands/_includes/conn_options_ref.md) %}

## Параметры командной строки {#pars}

`[options]` - параметры команды:

### Параметры соединения с S3 {#s3-conn}

Команда выгрузки в S3 требует указания [параметров соединения с S3](../s3_conn.md). Так как выгрузка производится в асинхронном режиме сервером YDB, указанный эндпоинт должен быть доступен для установки соединения со стороны сервера.

### Перечень выгружаемых объектов {#items}

`--item STRING`: Описание объекта выгрузки. Параметр `--item` может быть указан несколько раз, если необходимо выполнить выгрузку нескольких объектов. `STRING` задается в формате `<свойство>=<значение>,...`, со следующими обязательными свойствами:
- `source`, `src`, или `s` — путь до выгружаемой директории или таблицы, `.` указывает на корневую директорию базы данных. При указании директории выгружаются все объекты в ней, имена которых не начинаются с точки, а также рекурсивно все поддиректории, имена которых не начинаются с точки.
- `destination`, `dst`, или `d` —  путь (префикс ключа) в S3 для размещения выгружаемых объектов

`--exclude STRING`: Шаблон ([PCRE](https://www.pcre.org/original/doc/html/pcrepattern.html)) для исключения путей из выгрузки. Данный параметр может быть указан несколько раз, для разных шаблонов.

### Дополнительные параметры {#aux}

`--description STRING`: Текстовое описание операции, сохраняемое в истории операций
`--retries NUM`: Количество повторных попыток выгрузки, которые будет предпринимать сервер. По умолчанию 10.
`--format STRING`: Формат вывода результата
- `pretty`: Человекочитаемый формат (по умолчанию)
- `proto-json-base64`: Protobuf в формате json, бинарные строки закодированы в base64

## Выполнение выгрузки {#exec}

### Результат запуска {#result}

При успешном исполнении команда `export s3` выводит сводную информацию о поставленной в очередь операции выгрузки в S3, в заданном опцией `--format` формате. Фактическая выгрузка производится сервером асинхронно. В сводной информации выводится ID операции, который может быть использован в дальнейшем для проверки статуса и действий с операцией:

- В режиме вывода `pretty` (по умолчанию) идентификатор операции показывается в выделенном псевдографикой поле id:

  ```
  ┌───────────────────────────────────────────┬───────┬─────...
  | id                                        | ready | stat...
  ├───────────────────────────────────────────┼───────┼─────...
  | ydb://export/6?id=281474976788395&kind=s3 | true  | SUCC...
  ├╴╴╴╴╴╴╴╴╴╴╴╴╴╴╴╴╴╴╴╴╴╴╴╴╴╴╴╴╴╴╴╴╴╴╴╴╴╴╴╴╴╴╴┴╴╴╴╴╴╴╴┴╴╴╴╴╴...
  | StorageClass: NOT_SET                                      
  | Items:
  ...                                                   
  ```

- В режиме вывода proto-json-base64 идентификатор находится в атрибуте "id":

  ```
  {"id":"ydb://export/6?id=281474976788395&kind=s3","ready":true, ... }
  ```

### Статус выгрузки {#status}

Выгрузка данных выполняется в фоновом режиме. Получить информацию о статусе и прогрессе выгрузки можно вызовом команды `operation get`, параметром которой должен быть передан **заключенный в кавычки** идентификатор операции, например:

``` bash
{{ ydb-cli }} -p db1 operation get "ydb://export/6?id=281474976788395&kind=s3"
```

Формат вывода `operation get` также устанавливается опцией `--format`.

Несмотря на то, что идентификатор операции имеет формат URL, не гарантируется, что он будет сохранен в дальнейшем. Его нужно интерпретировать только как строку.

Завершение выгрузки отслеживается по изменению атрибута "progress":

- В режиме вывода `pretty` (по умолчанию) успешно завершенная операция отражается значением "Done" в выделенном псевдографикой поле `progress`:

  ```
  ┌───── ... ──┬───────┬─────────┬──────────┬─...
  | id         | ready | status  | progress | ...
  ├──────... ──┼───────┼─────────┼──────────┼─...
  | ydb:/...   | true  | SUCCESS | Done     | ...
  ├╴╴╴╴╴ ... ╴╴┴╴╴╴╴╴╴╴┴╴╴╴╴╴╴╴╴╴┴╴╴╴╴╴╴╴╴╴╴┴╴...
  ...
  ```

- В режиме вывода proto-json-base64 завершенная операция отражается значением `PROGRESS_DONE` атрибута `progress`:

  ```
  {"id":"ydb://...", ...,"progress":"PROGRESS_DONE",... }
  ```

### Завершение операции выгрузки {#forget}

При выполнении выгрузки в корневом каталоге базы данных создается директория с именем `export_*`, где `*` -- это числовая часть идентификатора выгрузки. В данной директории размещаются таблицы, содержащие консистентный снепшот выгружаемых данных на момент начала выгрузки.

После выполнения выгрузки воспользуйтесь командой `operation forget` для того, чтобы выгрузка была завершена: удалена из перечня операций, а также были удалены все созданные для неё файлы:

``` bash
{{ ydb-cli }} -p db1 operation forget "ydb://export/6?id=281474976788395&kind=s3"
```

### Список операций выгрузки {#list}

Для получения списка операций выгрузки воспользуйтесь командой `operation list export/s3`:

``` bash
{{ ydb-cli }} -p db1 operation list export/s3
```

Формат вывода `operation list` также устанавливается опцией `--format`.

## Примеры {#examples}

{% include [example_db1.md](../../_includes/example_db1.md) %}

### Выгрузка базы данных {#example-full-db}

Выгрузка всех объектов базы данных, имена которых не начинаются с точки, и не размещенных внутри директорий, имена которых начинаются с точки, в директорию `export1` в бакете `mybucket` с использованием параметров аутентификации S3 из переменных окружения или файла `~/.aws/credentials`:

```
ydb -p db1 export s3 \
  --s3-endpoint storage.yandexcloud.net --bucket mybucket \
  --item src=.,dst=export1
```

### Выгрузка нескольких директорий {#example-specific-dirs}

Выгрузка объектов из директорий dir1 и dir2 базы данных, в директорию `export1` в бакете `mybucket`, с использованием явно заданных параметров аутентификации в S3:

```
ydb -p db1 export s3 \
  --s3-endpoint storage.yandexcloud.net --bucket mybucket \
  --access-key VJGSOScgs-5kDGeo2hO9 --secret-key fZ_VB1Wi5-fdKSqH6074a7w0J4X0 \
  --item src=dir1,dst=export1/dir1 --item src=dir2,dst=export1/dir2
```

### Получение идентификаторов операций {#example-list-oneline}

Для получения перечня идентификаторов операций выгрузки в удобном для обработки в скриптах bash формате вы можете применить утилиту [jq](https://stedolan.github.io/jq/download/):

``` bash
{{ ydb-cli }} -p db1 operation list export/s3 --format proto-json-base64 | jq -r ".operations[].id"
```

Вы получите вывод, где в каждой новой строке находится идентификатор операции, например:

```
ydb://export/6?id=281474976789577&kind=s3
ydb://export/6?id=281474976789526&kind=s3
ydb://export/6?id=281474976788779&kind=s3
```

По этим идентификаторам может быть, например, запущен цикл для завершения всех текущих операций:

``` bash
{{ ydb-cli }} -p db1 operation list export/s3 --format proto-json-base64 | jq -r ".operations[].id" | while read line; do {{ ydb-cli }} -p db1 operation forget $line;done
```

