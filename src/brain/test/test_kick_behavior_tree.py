"""Check that match decisions actually reach a motion node in the shipped tree."""
import ast
import re
from pathlib import Path
import unittest
import xml.etree.ElementTree as ET


TREES = Path(__file__).resolve().parents[1] / "behavior_trees" / "subtrees"


def condition_matches(expression, variables):
    expression = expression.replace("||", " or ").replace("&&", " and ")
    expression = re.sub(r"!(?!=)", "not ", expression).strip()
    parsed = ast.parse(expression, mode="eval")
    allowed = (ast.Expression, ast.BoolOp, ast.Or, ast.And, ast.Compare,
               ast.Eq, ast.NotEq, ast.Name, ast.Load, ast.Constant, ast.UnaryOp, ast.Not)
    if any(not isinstance(node, allowed) for node in ast.walk(parsed)):
        raise ValueError(f"Unsupported motion condition: {expression}")
    return bool(eval(compile(parsed, "<BT condition>", "eval"), {"__builtins__": {}}, variables))


def selected_actions(node, variable, value, action_types):
    condition = node.get("_while")
    if condition and variable in condition and not condition_matches(condition, {variable: value}):
        return []
    actions = [node.tag] if node.tag in action_types else []
    for child in node:
        actions.extend(selected_actions(child, variable, value, action_types))
    return actions


class MatchMotionRouting(unittest.TestCase):
    def test_search_does_not_preempt_a_known_ball(self):
        for filename in ("subtree_striker_play.xml", "subtree_striker_freekick.xml"):
            root = ET.parse(TREES / filename).getroot()
            parents = {child: node for node in root.iter() for child in node}
            search = root.find(".//MoveToFarSetPlaySearchArea")
            condition = search.get("_while") or parents[search].get("_while")
            for local, shared in ((True, False), (False, True), (True, True), (False, False)):
                with self.subTest(filename=filename, local=local, shared=shared):
                    self.assertEqual(condition_matches(condition, {
                        "gc_opponent_set_play_search_direction": 1,
                        "ball_location_known": local, "tm_ball_pos_reliable": shared}),
                        not (local or shared))

    def test_ready_routes_to_formation_with_tight_arrival_tolerance(self):
        root = ET.parse(TREES.parent / "game.xml").getroot()
        ready = root.find(".//ReactiveSequence[@_while=\"gc_game_state=='READY'\"]")
        self.assertIsNotNone(ready)
        motion = {"GoToReadyPosition", "YieldBallSpace", "SetVelocity"}
        self.assertEqual(selected_actions(ready, "gc_game_state", "READY", motion),
                         ["GoToReadyPosition"])
        self.assertEqual(selected_actions(ready, "gc_game_state", "PLAY", motion), [])
        node = ready.find("GoToReadyPosition")
        self.assertLessEqual(float(node.get("dist_tolerance")), 0.25)
        self.assertEqual(ready.find("SubTree").get("ID"), "Locate")

    def test_yield_preempts_both_roles(self):
        root = ET.parse(TREES.parent / "game.xml").getroot()
        match = root.find(".//ReactiveSequence[@name='比赛']")
        self.assertIsNotNone(match)
        self.assertEqual(match[0].get("_while"), "yield_ball_space")
        self.assertEqual(match[1].get("_while"), "!yield_ball_space")
        self.assertEqual([n.tag for n in match[0]], ["SubTree", "YieldBallSpace"])
        for yielding in (True, False):
            for role, expected in (("striker", "StrikerPlay"), ("goal_keeper", "GoalKeeperPlay")):
                with self.subTest(yielding=yielding, role=role):
                    states = {"yield_ball_space": yielding, "player_role": role}
                    actions = []
                    def visit(node):
                        if node.get("_while") and not condition_matches(node.get("_while"), states):
                            return
                        if node.tag == "YieldBallSpace": actions.append(node.tag)
                        elif node.tag == "SubTree" and node.get("ID") in ("StrikerPlay", "GoalKeeperPlay"):
                            actions.append(node.get("ID"))
                        for child in node: visit(child)
                    for branch in match: visit(branch)
                    self.assertEqual(actions, ["YieldBallSpace" if yielding else expected])

    def test_striker_decisions_have_one_action(self):
        tree = ET.parse(TREES / "subtree_striker_play.xml").getroot()
        parents = {child: node for node in tree.iter() for child in node}
        decision_node = tree.find(".//StrikerDecide")
        tree = parents[decision_node]
        expected = {
            "find": "RobotFindBall", "assist": "Assist", "kickoff_hold": "KickoffStand",
            "chase": "Chase", "reposition": "Chase", "adjust": "Adjust", "auto_visual_kick": "RLVisionKick",
            "power_kick": "Kick", "kick": "Kick", "cross": "Kick", "safe_shoot": "Kick",
            "kick_wait": "SetVelocity",
        }
        for decision, action in expected.items():
            with self.subTest(decision=decision):
                self.assertEqual(selected_actions(tree, "decision", decision, set(expected.values())), [action])

    def test_reposition_passes_its_mode_to_the_motion_node(self):
        root = ET.parse(TREES / "subtree_striker_play.xml").getroot()
        chase = root.find(".//Chase")
        self.assertEqual(chase.get("mode"), "{decision}")
        self.assertTrue(condition_matches(chase.get("_while"), {"decision": "reposition"}))
        self.assertGreaterEqual(float(chase.get("dist")), 0.35)
        self.assertGreaterEqual(float(chase.get("safe_dist")), float(chase.get("dist")))

    def test_running_kick_does_not_skip_new_decisions(self):
        root = ET.parse(TREES / "subtree_striker_play.xml").getroot()
        parents = {child: node for node in root.iter() for child in node}
        decision = root.find(".//StrikerDecide")
        for action in root.iter():
            if action.tag not in ("Kick", "RLVisionKick"):
                continue
            ancestor = parents[action]
            while decision not in ancestor.iter():
                ancestor = parents[ancestor]
            self.assertEqual(ancestor.tag, "ReactiveSequence")
            # All async motions are dispatched through one direct child so
            # ReactiveSequence versions that enforce this rule stay compatible.
            self.assertEqual([child.tag for child in ancestor], ["StrikerDecide", "Sequence"])

    def test_goalie_modes_have_one_action(self):
        root = ET.parse(TREES / "subtree_goal_keeper_play.xml").getroot()
        tree = root.find(".//ReactiveSequence[@_while='!ball_out']")
        self.assertIsNotNone(tree)
        expected = {
            "clear_approach": "Chase", "clear_align": "Adjust", "block_kick": "RLVisionKick",
            "intercept_kick": "RLVisionKick", "guard_idle": "GoalieIdleHome", "guard_defend": "Intercept2",
            "clear_wait": "SetVelocity",
        }
        for mode, action in expected.items():
            with self.subTest(mode=mode):
                self.assertEqual(selected_actions(tree, "goalie_mode", mode, set(expected.values())), [action])


if __name__ == "__main__":
    unittest.main()
