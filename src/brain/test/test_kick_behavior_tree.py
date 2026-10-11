"""Check that match decisions actually reach a motion node in the shipped tree."""
import ast
from pathlib import Path
import unittest
import xml.etree.ElementTree as ET


TREES = Path(__file__).resolve().parents[1] / "behavior_trees" / "subtrees"


def condition_matches(expression, variables):
    expression = expression.replace("||", " or ").replace("&&", " and ")
    parsed = ast.parse(expression, mode="eval")
    allowed = (ast.Expression, ast.BoolOp, ast.Or, ast.And, ast.Compare,
               ast.Eq, ast.NotEq, ast.Name, ast.Load, ast.Constant)
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
    def test_striker_decisions_have_one_action(self):
        tree = ET.parse(TREES / "subtree_striker_play.xml").getroot()
        expected = {
            "find": "RobotFindBall", "assist": "Assist", "kickoff_hold": "KickoffStand",
            "chase": "Chase", "adjust": "Adjust", "auto_visual_kick": "RLVisionKick",
            "power_kick": "Kick", "kick": "Kick", "cross": "Kick", "safe_shoot": "Kick",
        }
        for decision, action in expected.items():
            with self.subTest(decision=decision):
                self.assertEqual(selected_actions(tree, "decision", decision, set(expected.values())), [action])

    def test_goalie_modes_have_one_action(self):
        tree = ET.parse(TREES / "subtree_goal_keeper_play.xml").getroot()
        expected = {
            "clear_approach": "Chase", "clear_align": "Adjust", "block_kick": "RLVisionKick",
            "intercept_kick": "RLVisionKick", "guard_idle": "GoalieIdleHome", "guard_defend": "Intercept2",
        }
        for mode, action in expected.items():
            with self.subTest(mode=mode):
                self.assertEqual(selected_actions(tree, "goalie_mode", mode, set(expected.values())), [action])


if __name__ == "__main__":
    unittest.main()
