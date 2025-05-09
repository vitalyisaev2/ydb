import abc
import logging
import enum
import functools
from typing import (
    Iterable,
    Optional,
)

from .. import (
    _apis,
    issues,
)
from .._grpc.grpcwrapper import ydb_topic as _ydb_topic
from .._grpc.grpcwrapper import ydb_query as _ydb_query
from ..connection import _RpcState as RpcState

from . import base
from ..settings import BaseRequestSettings

logger = logging.getLogger(__name__)


class QueryTxStateEnum(enum.Enum):
    NOT_INITIALIZED = "NOT_INITIALIZED"
    BEGINED = "BEGINED"
    COMMITTED = "COMMITTED"
    ROLLBACKED = "ROLLBACKED"
    DEAD = "DEAD"


class QueryTxStateHelper(abc.ABC):
    _VALID_TRANSITIONS = {
        QueryTxStateEnum.NOT_INITIALIZED: [
            QueryTxStateEnum.BEGINED,
            QueryTxStateEnum.DEAD,
            QueryTxStateEnum.COMMITTED,
            QueryTxStateEnum.ROLLBACKED,
        ],
        QueryTxStateEnum.BEGINED: [QueryTxStateEnum.COMMITTED, QueryTxStateEnum.ROLLBACKED, QueryTxStateEnum.DEAD],
        QueryTxStateEnum.COMMITTED: [],
        QueryTxStateEnum.ROLLBACKED: [],
        QueryTxStateEnum.DEAD: [],
    }

    _SKIP_TRANSITIONS = {
        QueryTxStateEnum.NOT_INITIALIZED: [],
        QueryTxStateEnum.BEGINED: [],
        QueryTxStateEnum.COMMITTED: [QueryTxStateEnum.COMMITTED, QueryTxStateEnum.ROLLBACKED],
        QueryTxStateEnum.ROLLBACKED: [QueryTxStateEnum.COMMITTED, QueryTxStateEnum.ROLLBACKED],
        QueryTxStateEnum.DEAD: [],
    }

    @classmethod
    def valid_transition(cls, before: QueryTxStateEnum, after: QueryTxStateEnum) -> bool:
        return after in cls._VALID_TRANSITIONS[before]

    @classmethod
    def should_skip(cls, before: QueryTxStateEnum, after: QueryTxStateEnum) -> bool:
        return after in cls._SKIP_TRANSITIONS[before]

    @classmethod
    def terminal(cls, state: QueryTxStateEnum) -> bool:
        return len(cls._VALID_TRANSITIONS[state]) == 0


def reset_tx_id_handler(func):
    @functools.wraps(func)
    def decorator(
        rpc_state, response_pb, session_state: base.IQuerySessionState, tx_state: QueryTxState, *args, **kwargs
    ):
        try:
            return func(rpc_state, response_pb, session_state, tx_state, *args, **kwargs)
        except issues.Error:
            tx_state._change_state(QueryTxStateEnum.DEAD)
            tx_state.tx_id = None
            raise

    return decorator


class QueryTxState:
    def __init__(self, tx_mode: base.BaseQueryTxMode):
        """
        Holds transaction context manager info
        :param tx_mode: A mode of transaction
        """
        self.tx_id = None
        self.tx_mode = tx_mode
        self._state = QueryTxStateEnum.NOT_INITIALIZED

    def _check_invalid_transition(self, target: QueryTxStateEnum) -> None:
        if not QueryTxStateHelper.valid_transition(self._state, target):
            raise RuntimeError(f"Transaction could not be moved from {self._state.value} to {target.value}")

    def _change_state(self, target: QueryTxStateEnum) -> None:
        self._check_invalid_transition(target)
        self._state = target

    def _check_tx_ready_to_use(self) -> None:
        if QueryTxStateHelper.terminal(self._state):
            raise RuntimeError(f"Transaction is in terminal state: {self._state.value}")

    def _should_skip(self, target: QueryTxStateEnum) -> bool:
        return QueryTxStateHelper.should_skip(self._state, target)


def _construct_tx_settings(tx_state: QueryTxState) -> _ydb_query.TransactionSettings:
    tx_settings = _ydb_query.TransactionSettings.from_public(tx_state.tx_mode)
    return tx_settings


def _create_begin_transaction_request(
    session_state: base.IQuerySessionState, tx_state: QueryTxState
) -> _apis.ydb_query.BeginTransactionRequest:
    request = _ydb_query.BeginTransactionRequest(
        session_id=session_state.session_id,
        tx_settings=_construct_tx_settings(tx_state),
    ).to_proto()
    return request


def _create_commit_transaction_request(
    session_state: base.IQuerySessionState, tx_state: QueryTxState
) -> _apis.ydb_query.CommitTransactionRequest:
    request = _apis.ydb_query.CommitTransactionRequest()
    request.tx_id = tx_state.tx_id
    request.session_id = session_state.session_id
    return request


def _create_rollback_transaction_request(
    session_state: base.IQuerySessionState, tx_state: QueryTxState
) -> _apis.ydb_query.RollbackTransactionRequest:
    request = _apis.ydb_query.RollbackTransactionRequest()
    request.tx_id = tx_state.tx_id
    request.session_id = session_state.session_id
    return request


@base.bad_session_handler
def wrap_tx_begin_response(
    rpc_state: RpcState,
    response_pb: _apis.ydb_query.BeginTransactionResponse,
    session_state: base.IQuerySessionState,
    tx_state: QueryTxState,
    tx: "BaseQueryTxContext",
) -> "BaseQueryTxContext":
    message = _ydb_query.BeginTransactionResponse.from_proto(response_pb)
    issues._process_response(message.status)
    tx_state._change_state(QueryTxStateEnum.BEGINED)
    tx_state.tx_id = message.tx_meta.tx_id
    return tx


@base.bad_session_handler
@reset_tx_id_handler
def wrap_tx_commit_response(
    rpc_state: RpcState,
    response_pb: _apis.ydb_query.CommitTransactionResponse,
    session_state: base.IQuerySessionState,
    tx_state: QueryTxState,
    tx: "BaseQueryTxContext",
) -> "BaseQueryTxContext":
    message = _ydb_query.CommitTransactionResponse.from_proto(response_pb)
    issues._process_response(message.status)
    tx_state._change_state(QueryTxStateEnum.COMMITTED)
    return tx


@base.bad_session_handler
@reset_tx_id_handler
def wrap_tx_rollback_response(
    rpc_state: RpcState,
    response_pb: _apis.ydb_query.RollbackTransactionResponse,
    session_state: base.IQuerySessionState,
    tx_state: QueryTxState,
    tx: "BaseQueryTxContext",
) -> "BaseQueryTxContext":
    message = _ydb_query.RollbackTransactionResponse.from_proto(response_pb)
    issues._process_response(message.status)
    tx_state._change_state(QueryTxStateEnum.ROLLBACKED)
    return tx


class BaseQueryTxContext(base.CallbackHandler):
    def __init__(self, driver, session_state, session, tx_mode):
        """
        An object that provides a simple transaction context manager that allows statements execution
        in a transaction. You don't have to open transaction explicitly, because context manager encapsulates
        transaction control logic, and opens new transaction if:

        1) By explicit .begin() method;
        2) On execution of a first statement, which is strictly recommended method, because that avoids useless round trip

        This context manager is not thread-safe, so you should not manipulate on it concurrently.

        :param driver: A driver instance
        :param session_state: A state of session
        :param tx_mode: Transaction mode, which is a one from the following choices:
         1) QuerySerializableReadWrite() which is default mode;
         2) QueryOnlineReadOnly(allow_inconsistent_reads=False);
         3) QuerySnapshotReadOnly();
         4) QueryStaleReadOnly().
        """

        self._driver = driver
        self._tx_state = QueryTxState(tx_mode)
        self._session_state = session_state
        self.session = session
        self._prev_stream = None
        self._external_error = None
        self._last_query_stats = None

    @property
    def session_id(self) -> str:
        """
        A transaction's session id

        :return: A transaction's session id
        """
        return self._session_state.session_id

    @property
    def tx_id(self) -> Optional[str]:
        """
        Returns an id of open transaction or None otherwise

        :return: An id of open transaction or None otherwise
        """
        return self._tx_state.tx_id

    @property
    def last_query_stats(self):
        return self._last_query_stats

    def _tx_identity(self) -> _ydb_topic.TransactionIdentity:
        if not self.tx_id:
            raise RuntimeError("Unable to get tx identity without started tx.")
        return _ydb_topic.TransactionIdentity(self.tx_id, self.session_id)

    def _set_external_error(self, exc: BaseException) -> None:
        self._external_error = exc

    def _check_external_error_set(self):
        if self._external_error is None:
            return
        raise issues.ClientInternalError("Transaction was failed by external error.") from self._external_error

    def _begin_call(self, settings: Optional[BaseRequestSettings]) -> "BaseQueryTxContext":
        self._tx_state._check_invalid_transition(QueryTxStateEnum.BEGINED)

        return self._driver(
            _create_begin_transaction_request(self._session_state, self._tx_state),
            _apis.QueryService.Stub,
            _apis.QueryService.BeginTransaction,
            wrap_tx_begin_response,
            settings,
            (self._session_state, self._tx_state, self),
        )

    def _commit_call(self, settings: Optional[BaseRequestSettings]) -> "BaseQueryTxContext":
        self._check_external_error_set()
        self._tx_state._check_invalid_transition(QueryTxStateEnum.COMMITTED)

        return self._driver(
            _create_commit_transaction_request(self._session_state, self._tx_state),
            _apis.QueryService.Stub,
            _apis.QueryService.CommitTransaction,
            wrap_tx_commit_response,
            settings,
            (self._session_state, self._tx_state, self),
        )

    def _rollback_call(self, settings: Optional[BaseRequestSettings]) -> "BaseQueryTxContext":
        self._check_external_error_set()
        self._tx_state._check_invalid_transition(QueryTxStateEnum.ROLLBACKED)

        return self._driver(
            _create_rollback_transaction_request(self._session_state, self._tx_state),
            _apis.QueryService.Stub,
            _apis.QueryService.RollbackTransaction,
            wrap_tx_rollback_response,
            settings,
            (self._session_state, self._tx_state, self),
        )

    def _execute_call(
        self,
        query: str,
        parameters: Optional[dict],
        commit_tx: Optional[bool],
        syntax: Optional[base.QuerySyntax],
        exec_mode: Optional[base.QueryExecMode],
        stats_mode: Optional[base.QueryStatsMode],
        concurrent_result_sets: Optional[bool],
        settings: Optional[BaseRequestSettings],
    ) -> Iterable[_apis.ydb_query.ExecuteQueryResponsePart]:
        self._tx_state._check_tx_ready_to_use()
        self._check_external_error_set()

        self._last_query_stats = None

        request = base.create_execute_query_request(
            query=query,
            parameters=parameters,
            commit_tx=commit_tx,
            session_id=self._session_state.session_id,
            tx_id=self._tx_state.tx_id,
            tx_mode=self._tx_state.tx_mode,
            syntax=syntax,
            exec_mode=exec_mode,
            stats_mode=stats_mode,
            concurrent_result_sets=concurrent_result_sets,
        )

        return self._driver(
            request.to_proto(),
            _apis.QueryService.Stub,
            _apis.QueryService.ExecuteQuery,
            settings=settings,
        )

    def _move_to_beginned(self, tx_id: str) -> None:
        if self._tx_state._should_skip(QueryTxStateEnum.BEGINED) or not tx_id:
            return
        self._tx_state._change_state(QueryTxStateEnum.BEGINED)
        self._tx_state.tx_id = tx_id

    def _move_to_commited(self) -> None:
        if self._tx_state._should_skip(QueryTxStateEnum.COMMITTED):
            return
        self._tx_state._change_state(QueryTxStateEnum.COMMITTED)


class QueryTxContext(BaseQueryTxContext):
    def __init__(self, driver, session_state, session, tx_mode):
        """
        An object that provides a simple transaction context manager that allows statements execution
        in a transaction. You don't have to open transaction explicitly, because context manager encapsulates
        transaction control logic, and opens new transaction if:

        1) By explicit .begin() method;
        2) On execution of a first statement, which is strictly recommended method, because that avoids useless round trip

        This context manager is not thread-safe, so you should not manipulate on it concurrently.

        :param driver: A driver instance
        :param session_state: A state of session
        :param tx_mode: Transaction mode, which is a one from the following choices:
         1) QuerySerializableReadWrite() which is default mode;
         2) QueryOnlineReadOnly(allow_inconsistent_reads=False);
         3) QuerySnapshotReadOnly();
         4) QueryStaleReadOnly().
        """

        super().__init__(driver, session_state, session, tx_mode)
        self._init_callback_handler(base.CallbackHandlerMode.SYNC)

    def __enter__(self) -> "BaseQueryTxContext":
        """
        Enters a context manager and returns a transaction

        :return: A transaction instance
        """
        return self

    def __exit__(self, *args, **kwargs):
        """
        Closes a transaction context manager and rollbacks transaction if
        it is not finished explicitly
        """
        self._ensure_prev_stream_finished()
        if self._tx_state._state == QueryTxStateEnum.BEGINED and self._external_error is None:
            # It's strictly recommended to close transactions directly
            # by using commit_tx=True flag while executing statement or by
            # .commit() or .rollback() methods, but here we trying to do best
            # effort to avoid useless open transactions
            logger.warning("Potentially leaked tx: %s", self._tx_state.tx_id)
            try:
                self.rollback()
            except issues.Error:
                logger.warning("Failed to rollback leaked tx: %s", self._tx_state.tx_id)

    def _ensure_prev_stream_finished(self) -> None:
        if self._prev_stream is not None:
            with self._prev_stream:
                pass
            self._prev_stream = None

    def begin(self, settings: Optional[BaseRequestSettings] = None) -> "QueryTxContext":
        """Explicitly begins a transaction

        :param settings: An additional request settings BaseRequestSettings;

        :return: Transaction object or exception if begin is failed
        """
        self._begin_call(settings)

        return self

    def commit(self, settings: Optional[BaseRequestSettings] = None) -> None:
        """Calls commit on a transaction if it is open otherwise is no-op. If transaction execution
        failed then this method raises PreconditionFailed.

        :param settings: An additional request settings BaseRequestSettings;

        :return: A committed transaction or exception if commit is failed
        """
        self._check_external_error_set()
        if self._tx_state._should_skip(QueryTxStateEnum.COMMITTED):
            return

        if self._tx_state._state == QueryTxStateEnum.NOT_INITIALIZED:
            self._tx_state._change_state(QueryTxStateEnum.COMMITTED)
            return

        self._ensure_prev_stream_finished()

        try:
            self._execute_callbacks_sync(base.TxEvent.BEFORE_COMMIT)
            self._commit_call(settings)
            self._execute_callbacks_sync(base.TxEvent.AFTER_COMMIT, exc=None)
        except BaseException as e:  # TODO: probably should be less wide
            self._execute_callbacks_sync(base.TxEvent.AFTER_COMMIT, exc=e)
            raise e

    def rollback(self, settings: Optional[BaseRequestSettings] = None) -> None:
        """Calls rollback on a transaction if it is open otherwise is no-op. If transaction execution
        failed then this method raises PreconditionFailed.

        :param settings: An additional request settings BaseRequestSettings;

        :return: A committed transaction or exception if commit is failed
        """
        self._check_external_error_set()
        if self._tx_state._should_skip(QueryTxStateEnum.ROLLBACKED):
            return

        if self._tx_state._state == QueryTxStateEnum.NOT_INITIALIZED:
            self._tx_state._change_state(QueryTxStateEnum.ROLLBACKED)
            return

        self._ensure_prev_stream_finished()

        try:
            self._execute_callbacks_sync(base.TxEvent.BEFORE_ROLLBACK)
            self._rollback_call(settings)
            self._execute_callbacks_sync(base.TxEvent.AFTER_ROLLBACK, exc=None)
        except BaseException as e:  # TODO: probably should be less wide
            self._execute_callbacks_sync(base.TxEvent.AFTER_ROLLBACK, exc=e)
            raise e

    def execute(
        self,
        query: str,
        parameters: Optional[dict] = None,
        commit_tx: Optional[bool] = False,
        syntax: Optional[base.QuerySyntax] = None,
        exec_mode: Optional[base.QueryExecMode] = None,
        concurrent_result_sets: Optional[bool] = False,
        settings: Optional[BaseRequestSettings] = None,
        *,
        stats_mode: Optional[base.QueryStatsMode] = None,
    ) -> base.SyncResponseContextIterator:
        """Sends a query to Query Service

        :param query: (YQL or SQL text) to be executed.
        :param parameters: dict with parameters and YDB types;
        :param commit_tx: A special flag that allows transaction commit.
        :param syntax: Syntax of the query, which is a one from the following choices:
         1) QuerySyntax.YQL_V1, which is default;
         2) QuerySyntax.PG.
        :param exec_mode: Exec mode of the query, which is a one from the following choices:
         1) QueryExecMode.EXECUTE, which is default;
         2) QueryExecMode.EXPLAIN;
         3) QueryExecMode.VALIDATE;
         4) QueryExecMode.PARSE.
        :param concurrent_result_sets: A flag to allow YDB mix parts of different result sets. Default is False;
        :param settings: An additional request settings BaseRequestSettings;
        :param stats_mode: Mode of query statistics to gather, which is a one from the following choices:
         1) QueryStatsMode:NONE, which is default;
         2) QueryStatsMode.BASIC;
         3) QueryStatsMode.FULL;
         4) QueryStatsMode.PROFILE;

        :return: Iterator with result sets
        """
        self._ensure_prev_stream_finished()

        stream_it = self._execute_call(
            query=query,
            commit_tx=commit_tx,
            syntax=syntax,
            exec_mode=exec_mode,
            stats_mode=stats_mode,
            parameters=parameters,
            concurrent_result_sets=concurrent_result_sets,
            settings=settings,
        )

        self._prev_stream = base.SyncResponseContextIterator(
            stream_it,
            lambda resp: base.wrap_execute_query_response(
                rpc_state=None,
                response_pb=resp,
                session_state=self._session_state,
                tx=self,
                commit_tx=commit_tx,
                settings=self.session._settings,
            ),
        )
        return self._prev_stream
