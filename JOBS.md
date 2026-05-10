Propose a plan to add tools:

open_job
  desc: Create a job ticket with an informative name and a detailed description, suitable for another agent to pick up.
  args: name (string, must be unique), description (detailed markdown document)

list_jobs
  desc: Return a list of open jobs names
  result: array of open job names

read_job
  desc: Retrieve an open job document.
  args: name
  result: combined markdown document: name + description + comments

comment_job
  desc: Append a comment to a job. Detailed as you like.
  args: name, new comment markdown

close_job:
  desc: Close a job. Removed from the list.
  args: name

Jobs are transient and not saved to disk. Could be just a map<string,???>.

Jobs are like software dev tickets with a task description and append-only comment threads, but intentionally simpler and named differently to avoid terminology mixups with github/gitlab/jira interactions.

The ImGUI window shall have a Jobs menu showing a list of jobs for the user to see.
Selecting a job from the menu opens a popup window with the full name + description + comments markdown document for perusal. Reuse render_content().


