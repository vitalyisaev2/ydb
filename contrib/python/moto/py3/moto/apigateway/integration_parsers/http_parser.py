import requests
from typing import Tuple, Union

from . import IntegrationParser
from ..models import Integration


class TypeHttpParser(IntegrationParser):
    """
    Parse invocations to a APIGateway resource with integration type HTTP
    """

    def invoke(
        self, request: requests.PreparedRequest, integration: Integration
    ) -> Tuple[int, Union[str, bytes]]:
        uri = integration["uri"]
        requests_func = getattr(requests, integration["httpMethod"].lower())
        response = requests_func(uri)
        return response.status_code, response.text
