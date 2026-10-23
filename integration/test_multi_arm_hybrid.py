"""Multi-arm RRF checked against independently ranked searches, locally and clustered."""
import struct

import pytest
from valkey_search_test_case import ValkeySearchTestCaseBase, ValkeySearchClusterTestCase
from valkeytestframework.conftest import resource_port_tracker  # noqa: F401
from valkeytestframework.util import waiters


def blob(x):
    return struct.pack('<2f', x, 0)


class MultiArmCases:
    def seed(self, client, writer):
        client.execute_command(
            'FT.CREATE', 'multi', 'ON', 'HASH', 'PREFIX', 1, 'multi:', 'SCHEMA',
            'title', 'TEXT',
            'a', 'VECTOR', 'FLAT', 6, 'TYPE', 'FLOAT32', 'DIM', 2, 'DISTANCE_METRIC', 'L2',
            'b', 'VECTOR', 'FLAT', 6, 'TYPE', 'FLOAT32', 'DIM', 2, 'DISTANCE_METRIC', 'L2')
        for i in range(12):
            writer.hset(f'multi:{i:02}', mapping={
                'title': 'apple' if i % 2 else 'pear', 'a': blob(i), 'b': blob(11-i)})
        waiters.wait_for_true(lambda: client.execute_command(
            'FT.SEARCH', 'multi', '*', 'LIMIT', 0, 0)[0] == 12, timeout=15)

    @pytest.mark.parametrize('shape', ['tt', 'vv', 'vtv', 'vtvtt'])
    def test_multi_arm_rrf_matches_independent_rankings(self, shape):
        client = self.setup_multi()
        arms = []
        expected = {}
        for i, kind in enumerate(shape):
            if kind == 'v':
                field = 'a' if i == 0 else 'b'
                term = 'apple' if i == 0 else 'pear'
                predicate = f'@title:{term}'
                arms += ['VSIM', f'@{field}', '$q', 'KNN', 2, 'K', 4,
                         'FILTER', predicate]
                query = f'*=>[KNN 12 @{field} $q]'
                reply = client.execute_command(
                    'FT.SEARCH', 'multi', query, 'NOCONTENT', 'PARAMS', 2, 'q', blob(0),
                    'LIMIT', 0, 12, 'DIALECT', 2)
                keys = [key for key in reply[1:]
                        if (int(key.split(b':')[1]) % 2 == 1) == (term == 'apple')]
            else:
                term = 'missing' if i == 4 else ('apple' if i % 2 else 'pear')
                query = f'@title:{term}'
                arms += ['SEARCH', query]
                reply = client.execute_command('FT.SEARCH', 'multi', query,
                                               'NOCONTENT', 'WITHSCORES', 'LIMIT', 0, 20)
                # Equal scores use document ID as the fusion tie break.
                ranked = [(reply[j], float(reply[j+1])) for j in range(1, len(reply), 2)]
                keys = [key for key, _ in sorted(ranked, key=lambda row: (-row[1], row[0]))]
            for rank, key in enumerate(keys[:4], 1):
                expected[key] = expected.get(key, 0) + 1 / (60 + rank)
        params = ['PARAMS', 2, 'q', blob(0)] if 'v' in shape else []
        reply = client.execute_command(
            'FT.HYBRID', 'multi', *arms, 'COMBINE', 'RRF', 2, 'WINDOW', 4,
            *params, 'APPLY', '@__score*2', 'AS', 'doubled', 'LIMIT', 0, 20)
        actual = {}
        for row in reply[1:]:
            fields = dict(zip(row[::2], row[1::2]))
            actual[fields[b'__key']] = float(fields[b'__score'])
            assert float(fields[b'doubled']) == pytest.approx(2 * float(fields[b'__score']))
        assert actual == pytest.approx(expected)

    def test_multi_arm_function_sums_arm_scores(self):
        client = self.setup_multi()
        expected = {}
        for field in ('a', 'b'):
            reply = client.execute_command(
                'FT.SEARCH', 'multi', f'*=>[KNN 12 @{field} $q]', 'RETURN', 1,
                f'__{field}_score', 'PARAMS', 2, 'q', blob(0), 'LIMIT', 0, 12,
                'DIALECT', 2)
            # Fusion sees an L2 distance d as the similarity 1 / (1 + d).
            for key, fields in zip(reply[1::2], reply[2::2]):
                expected[key] = expected.get(key, 0) + 1 / (1 + float(fields[1]))
        # Every document is in all three arms, so no term reads a missing score.
        reply = client.execute_command(
            'FT.HYBRID', 'multi',
            'VSIM', '@a', '$q', 'KNN', 2, 'K', 12, 'YIELD_SCORE_AS', 'va',
            'VSIM', '@b', '$q', 'KNN', 2, 'K', 12, 'YIELD_SCORE_AS', 'vb',
            'SEARCH', '*',
            'COMBINE', 'FUNCTION', 2, 'EXPR', '@va + @vb + @__arm2_score * 0',
            'PARAMS', 2, 'q', blob(0), 'LIMIT', 0, 20)
        actual = {}
        for row in reply[1:]:
            fields = dict(zip(row[::2], row[1::2]))
            actual[fields[b'__key']] = float(fields[b'__score'])
        assert len(actual) == 12
        assert actual == pytest.approx(expected)


class TestMultiArmHybrid(MultiArmCases, ValkeySearchTestCaseBase):
    def setup_multi(self):
        client = self.server.get_new_client()
        self.seed(client, client)
        return client


class TestMultiArmHybridCluster(MultiArmCases, ValkeySearchClusterTestCase):
    def setup_multi(self):
        client = self.new_client_for_primary(0)
        self.seed(client, self.new_cluster_client())
        return client
