# Agent Workflow

Two fixed agent roles share a global Plan document:

**Planner agent** (left panel, Planner tab):
- Has all tools including read_plan, write_plan, comment_plan
- Researches, designs plans, writes them to the Plan document via write_plan
- Reviews builder progress via read_plan and code inspection
- Adds comments using comment_plan for reviews / change requests

**Builder agent** (left panel, Builder tab):
- Has all tools except write_plan and job tools (read_plan + comment_plan only for plan)
- Reads the Plan document with read_plan to understand what to implement
- Implements code, runs tests, reports back via comment_plan
- Reviews new comments and iterates

Workflow:
1. User and Planner design the plan -> Planner calls write_plan
2. User switches to Builder tab -> Builder reads plan with read_plan
3. Builder implements, tests, calls comment_plan with summary
4. User switches to Planner -> Planner reviews, may call comment_plan with feedback
5. Builder reads new comments, iterates, repeats
6. Once done, Builder commits changes
7. User clears both sessions for next task
