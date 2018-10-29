# -*- coding: utf-8 -*-
from __future__ import absolute_import

import re
import json
import cherrypy
import mock

from .helper import ControllerTestCase
from ..controllers import RESTController, Controller
from ..tools import RequestLoggingTool, Session
from .. import mgr


# pylint: disable=W0613
@Controller('/foo', secure=False)
class FooResource(RESTController):
    def create(self, data):
        pass

    def get(self, key):
        pass

    def delete(self, key):
        pass

    def set(self, key, data):
        pass


class ApiAuditingTest(ControllerTestCase):
    settings = {}

    def __init__(self, *args, **kwargs):
        cherrypy.tools.request_logging = RequestLoggingTool()
        cherrypy.config.update({'tools.request_logging.on': True})
        super(ApiAuditingTest, self).__init__(*args, **kwargs)

    @classmethod
    def mock_set_config(cls, key, val):
        cls.settings[key] = val

    @classmethod
    def mock_get_config(cls, key, default):
        return cls.settings.get(key, default)

    @classmethod
    def setUpClass(cls):
        mgr.get_config.side_effect = cls.mock_get_config
        mgr.set_config.side_effect = cls.mock_set_config

    @classmethod
    def setup_server(cls):
        cls.setup_controllers([FooResource])

    def setUp(self):
        mgr.cluster_log = mock.Mock()
        mgr.set_config('AUDIT_API_ENABLED', True)
        mgr.set_config('AUDIT_API_LOG_PAYLOAD', True)

    def _validate_cluster_log_msg(self, path, method, user, params):
        channel, _, msg = mgr.cluster_log.call_args_list[0][0]
        self.assertEqual(channel, 'audit')
        pattern = r'^\[DASHBOARD\] from=\'(.+)\' path=\'(.+)\' ' \
                  'method=\'(.+)\' user=\'(.+)\' params=\'(.+)\'$'
        m = re.match(pattern, msg)
        self.assertEqual(m.group(2), path)
        self.assertEqual(m.group(3), method)
        self.assertEqual(m.group(4), user)
        self.assertDictEqual(json.loads(m.group(5)), params)

    def test_no_audit(self):
        mgr.set_config('AUDIT_API_ENABLED', False)
        self._delete('/foo/test1')
        mgr.cluster_log.assert_not_called()

    def test_no_payload(self):
        mgr.set_config('AUDIT_API_LOG_PAYLOAD', False)
        self._delete('/foo/test1')
        _, _, msg = mgr.cluster_log.call_args_list[0][0]
        self.assertNotIn('params=', msg)

    def test_no_audit_get(self):
        self._get('/foo/test1')
        mgr.cluster_log.assert_not_called()

    def test_audit_put(self):
        self._put('/foo/test1', {'data': 'y'})
        mgr.cluster_log.assert_called_once()
        self._validate_cluster_log_msg('/foo/test1', 'PUT', 'None',
                                       {'data': 'y', 'key': 'test1'})

    def test_audit_post(self):
        sess_mock = cherrypy.lib.sessions.RamSession()
        with mock.patch('cherrypy.session', sess_mock, create=True):
            cherrypy.session[Session.USERNAME] = 'hugo'
            self._post('/foo?data=x')
            mgr.cluster_log.assert_called_once()
            self._validate_cluster_log_msg('/foo', 'POST', 'hugo',
                                           {'data': 'x'})

    def test_audit_delete(self):
        self._delete('/foo/test1')
        mgr.cluster_log.assert_called_once()
        self._validate_cluster_log_msg('/foo/test1', 'DELETE',
                                       'None', {'key': 'test1'})
