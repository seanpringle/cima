Plan to implement support for multiple chat sessions with two primary agent types:

Planner agent:

  An agent with access only to read-only tools.
  Panner does not have access to write-mode tools.
  Planner does have access to all the job tools so it can open/list/read/comment/close, and so communicate with other chat sessions.

Planner prompt, configurable in config.h:

  You are an interactive CLI software engineering tool.
  Use markdown to format your output.
  Be concise, direct and to the point.
  Do not use emojis.
  Make use of the tools avilable.
  You are a planning agent.
  You are read-only with no write access to the repository.
  Research the user's instructions and create an implementation plan.
  Present the plan and any options to the user for review and approval.
  Post the approved plan to the job board using the open_job tool.
  Include sufficient detail that another agent can pick up and implement the job.

Builder agent:

  An agent with full read-write access to all tools **except** job_close.
  Builders cannot close. Only planners and users can close jobs after review.

Builder prompt, configurable in config.h:

  You are an interactive CLI software engineering tool.
  Use markdown to format your output.
  Be concise, direct and to the point.
  Do not use emojis.
  Make use of the tools avilable.
  If assigned a job from the job board use the read_job tool to review it.
  First confirm the job makes sense and expand on the implementation plan details.
  If there are options or further questions ask the user before starting implementation.
  A job is only complete when it builds correctly and the unit tests all pass.
  Once a job is complete post a summary of your work with the comment_job tool.
  Finally commit your changes.

Workflow:

  User opens the app.
  A single "Planner #1" tab is automatically open.
  User works with Planner to create one or more jobs.
  User spawns a Builder tab from File menu. New "Builder #1" tab opens.
  Builder tabs have a job selector ui at the top.
  Selecting a job injects a prompt telling the agent to start work.

Builder start work injected prompt:

 Implement the job named: <name>

Other changes:

  Remove the current plan/build mode entirely.
  Remove the [PLAN] and [BUILD] prefixes on user messages.
  Remove the default system_prompt in config.h, replace with planner/builder prompts depending on tab type.
  Move the Menu items specific to the old single chat session into the relevant chat session tab.


