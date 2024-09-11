import time
from datetime import datetime
from typing import Sequence
import pathlib

import yatest.common

from ydb.library.yql.providers.generic.connector.tests.utils.docker_compose import (
    DockerComposeHelper,
)
from ydb.library.yql.providers.generic.connector.tests.utils.log import make_logger

LOGGER = make_logger(__name__)


class YDB:
    __launched: bool = False

    def __init__(
        self, docker_compose_dir: pathlib.Path, expected_tables: Sequence[str]
    ):
        docker_compose_file_relative_path = str(
            docker_compose_dir / "docker-compose.yml"
        )
        docker_compose_file_abs_path = yatest.common.source_path(
            docker_compose_file_relative_path
        )
        self.docker_compose_helper = DockerComposeHelper(
            docker_compose_yml_path=docker_compose_file_abs_path
        )
        self.expected_tables = set(expected_tables)

    def wait(self):
        if self.__launched:
            return

        # This should be enough for tables to initialize
        start = datetime.now()

        timeout = 600
        while (datetime.now() - start).total_seconds() < timeout:
            self.actual_tables = set(self.docker_compose_helper.list_ydb_tables())

            # check if all the required tables have been created
            if self.expected_tables <= self.actual_tables:
                self.__launched = True
                return

            LOGGER.warning(
                f"Not enough YDB tables: expected={self.expected_tables}, actual={self.actual_tables}"
            )
            time.sleep(5)

        raise ValueError(
            f"YDB was not able to initialize in {timeout} seconds, latest table set: {self.actual_tables}"
        )
