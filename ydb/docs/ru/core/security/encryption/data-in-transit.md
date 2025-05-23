# Шифрование данных при передаче

Так как {{ ydb-short-name }} является распределённой системой, обычно работающей на кластере, часто расположенным в нескольких датацентрах или зонах доступности, пользовательские данные регулярно передаются по сети. Могут использоваться различные протоколы, каждый из которых может быть настроен для работы с использованием [TLS](https://en.wikipedia.org/wiki/Transport_Layer_Security). Ниже приведён список протоколов, поддерживаемых {{ ydb-short-name }}:

* [Интерконнект](../../concepts/glossary.md#actor-system-interconnect) — специализированный протокол для общения между узлами {{ ydb-short-name }}.
* {{ ydb-short-name }} в роли сервера:

  * [gRPC](../../reference/ydb-sdk/overview-grpc-api.md) — для внешнего взаимодействия с клиентскими приложениями, разработанными для нативной работы с {{ ydb-short-name }} через [SDK](../../reference/ydb-sdk/index.md) или [CLI](../../reference/ydb-cli/index.md).
  * [Протокол PostgreSQL](../../postgresql/intro.md) — для внешнего взаимодействия с клиентскими приложениями, изначально разработанными для работы с [PostgreSQL](https://www.postgresql.org/).
  * [Протокол Kafka](../../reference/kafka-api/index.md) — для внешнего взаимодействия с клиентскими приложениями, изначально разработанными для работы с [Apache Kafka](https://kafka.apache.org/).
  * HTTP — для работы с [встроенным UI](../../reference/embedded-ui/index.md), публикации [метрик](../../devops/observability/monitoring.md) и других вспомогательных конечных эндпоинтов.

* {{ ydb-short-name }} в роли клиента:

  * [LDAP](../authentication.md#ldap) — для аутентификации пользователей.
  * [Федеративные запросы](../../concepts/federated_query/index.md) — функциональность, позволяющая {{ ydb-short-name }} выполнять запросы к различным внешним источникам данных. Запросы к некоторым источникам отправляются напрямую из процесса `ydbd`, в то время как другие проксируются через отдельный процесс-коннектор.
  * [Трассировочные](../../reference/observability/tracing/setup.md) данные отправляются во внешний сборщик через gRPC.

{% if feature_async_replication == true %}

* В [асинхронной репликации](../../concepts/async-replication.md) между двумя базами данных {{ ydb-short-name }} одна из них выступает в роли клиента для другой.

{% endif %}

По умолчанию шифрование данных при передаче отключено и должно быть включено отдельно для каждого протокола. Они могут использовать общий набор TLS-сертификатов или отдельные. Инструкции по включению TLS можно найти в разделе [{#T}](../../reference/configuration/tls.md).
