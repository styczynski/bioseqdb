---
title: Probably the worst database ever — but hey it can write poems!
slug: chatdb-a-database-that-can-write-poems
description: My December 2022 ChatDB experiment, republished with its original wording, a new diagram and a separate note checking the old implementation.
date: '2026-10-12'
author: Piotr Styczyński
category: others
keywords: [PostgreSQL, databases, ChatDB, ChatGPT, key-value store, transaction log, SQL, experiments]
cover: /static/figures/blog/sequence-letters.svg
image_alt: Coloured DNA letters in small translucent boxes.
draft: false
---

*Originally published by me on [8 December 2022 on Medium](https://medium.com/@styczynski/probably-the-worst-database-ever-but-hey-it-can-write-poems-539b7757ad6d). These selected passages retain my original wording and jokes. The illustration is new, and the final section contains notes added for this republication. The original screenshots, long implementation listings and obsolete connection instructions are omitted; the old service integration has not been revalidated.*

Can your database write poems in the middle of the business operation? I bet not!

Let’s contruct something more useful. Something that would have real-world application, but still make model behave more like a machine. I decided to emulate database behvaiour. It can be then used to store some arbitrary information in the conversation. Maybe it can emulate very-limited PostgreSQL instance?

## A simple key-value store

Okay so start slowly. Instead of trying go full-SQL let’s create a simple key value store firstly. I decided to allow only javacsript code. But can we run javascript code?

We start by defining behaviour using functions that are simple enough to explain:

> I want you to act as a computer program. I will type javascript commands and return the result. I want you to reply with the value returned by execution of the code inside one code block. Do not write explanations.

We limit ourselves only to javascript execution

> Do not do anything, unless I tell you to do so. I can invoke function “save” that stores single value in memory. The function accepts key and value to be stored under this key. Keys are strings and any value other than string should cause an error.

That defines storage function `save(key: string, value: any) => any`

> I can call “read” which returns value stored under the key.

We obviously need storage reads and this fragment defines `read(key: string) => any`

> I also can invoke “all” which returns a list with all values stored as a json. I also can invoke “delete” which deletes a given key.

This is obviously for listing all keys and deletions:

```text
all() => Dict[string, any]
delete(key: string) => any
```

> I can also invoke “filter” which accepts regex and returns list of values for keys that match that regex. No other function other than those mentioned can be called.

Filter is more interesting. We can filter all keys using regex. The model should return list of values, but in practice this sometimes the definition not clear enough.

```text
filter(regex_to_match_keys: string) => List[any]
```

> If there is any error you should print it with line number and all the details. My first command is save(“abc”, 42);

Ah, that suprisingly worked! The model is no longer complaining about simulating a program. Perfect!

## Natural language and a Python wrapper

What about adding more interesting features? Let’s make our database listen to natural lanugage (why to bother with SQL ever again, when we can totally overload the model!)

> When I need you to tell something in English I will do so by calling function query that accepts a natural language input. For example query(“sum all values”) returns sum of all values.

We created function `query(natural_language: string) => any`

I love how the exact format of responses is defined by the model. Finally we can use our human-like ML model to model how human engineers behave when they get api requirements — everybody just come up with their idea of the exact implementation. We have our vague API ahh

The last part is:
Let’s wrap that into Python package so we can use the key-value store we have! I used simple pacakge created by [Antonio Cheong on GitHub](https://github.com/acheong08/ChatGPT). However I refreshed it a little bit and changed requirements.txt into poetry pyproject.lock, used aiohttp instead of requests and add few tiny little shiny things.

*2026 note: the [original demo](https://github.com/styczynski/chatdb/blob/bbeda04166ac506fb97df6fb88dc5af90b1e022c/demo.py) contains the initialization and the `print_assert` helper. This excerpt shows its historical expectations, not a new model run:*

```python
print_assert(await kv.save("a", 9))
print_assert(await kv.save("xx", 12))
print_assert(await kv.save("xy", 42))
print_assert(await kv.save("zz", 99))
print_assert(await kv.filter("x[x-y]"),
             ['xx', 'xy'], [12, 42], ["12", "42"])
print_assert(await kv.delete("xy"))
print_assert(await kv.query("sum all the values"), {'sum': 120}, 120)
```

`print_assert` is just a function that prints value returned by function and if it doesn’t match any of the provided values, the error is thrown. We need that, because standard asserts are not enough if the model sometimes yields results in different forms.

The code I created is available on GitHub: [styczynski/chatdb](https://github.com/styczynski/chatdb).

Now you can do crazy things like that:

*2026 note: this excerpt comes from the separate [poem demo](https://github.com/styczynski/chatdb/blob/bbeda04166ac506fb97df6fb88dc5af90b1e022c/poem_demo.py), after initializing a fresh store.*

```python
await kv.save("x", 42)
await kv.save("a", 19)
await kv.save("x", 99)
print(await kv.query("write me a short poem, two sentences long"))
print(await kv.all())
```

CAN YOUR REDIS INSTANCE WRITE YOU POEMS? Now I can have a little poem here and there when doing business logic of my enterprise application, perfect!

## Bonus time!

I thought to myself: Can we store somewhere logs of operations so we can revert the database to previous states? Hm, yes we can! Just hire an employee and tell him that:

> I want you to act as a computer program.
>
> I will type javascript commands and you should remember all of them in order. Do not do anything.
>
> After I type “SHOW” you should type exactly all the commands I told you, each command in new line.
>
> After I type “UNDO” you should remove the specified number of last commands.
> For example “UNDO 3” removes 3 last commands. Do not react to anything other than “SHOW” or “UNDO”.
>
> Do not write explanations.
> If I type anything other than “SHOW” or “UNDO”, respond with “OK”

We will submitt all commands to the logging ChatGTP instance and we can use SHOW to recreate fresh instance of the database in the existing state. We just ask for commands, spawn new ChatGPT and redo all operations or we can previously use UNDO to undo specific number of operations.

![The Python wrapper sends commands concurrently to a storage conversation and a log conversation. Undo shortens the log, resets the default storage conversation and replays the retained commands.](/assets/5429149deec7cd4e1a59f651da7355773835cbb8e8dcd9fdc292c9e9df4ff6f5/figures/blog/chatdb-conversations.svg)

That’s a nice architecture! Imagine creating a party, inviting two guests and telling them to behave like a KV storage and log respecitvely. That must be REAL GOOD PARTY. Altough, I hope chatGPT is not annoyed too much by doing those tedious data manipulation tasks.

## Final notes from 2022

Entire code used here is available on my GitHub. You can use the code to create your own “sentient” ChatGPT-backed key-value store.

The next step would be teaching chatGPT to emulate processor and running doom on it.

## Notes added in 2026

The commands above were text sent to a model, not JavaScript executed by an interpreter. The quoted prompt's restrictions and the old demo's accepted answers were also inconsistent: `filter` asked for values, while the demo accepted keys or values. The [wrapper](https://github.com/styczynski/chatdb/blob/bbeda04166ac506fb97df6fb88dc5af90b1e022c/chatgptdb/key_value_store.py) tries JSON parsing, then `ast.literal_eval`, then a plain string. Parseable output does not establish correct stored state.

The [log](https://github.com/styczynski/chatdb/blob/bbeda04166ac506fb97df6fb88dc5af90b1e022c/chatgptdb/action_log.py) counts commands, including reads. Normal commands go to the store and log concurrently, without an atomic commit between them. On the default path, `undo` trims the log, resets storage and replays retained commands without logging them again. Supplying existing conversation IDs disables the reset-on-start flag, so that path can replay into an existing conversation instead. These wrapper behaviours were checked offline with fake conversations, without contacting a model.

This differs from write-ahead logging: PostgreSQL requires the relevant WAL records to reach permanent storage before the corresponding data-file changes. The two conversations do not provide that durability rule. [PostgreSQL's WAL introduction](https://www.postgresql.org/docs/current/wal-intro.html) explains the difference.

For comparison, this ordinary PostgreSQL example uses synthetic sample counts. It runs during the website build and does not execute ChatDB:

```sql
UPDATE sample_counts SET value = 11
WHERE key = 'sample_A';

SELECT jsonb_object_agg(key, value ORDER BY key) AS stored_values,
       sum(value) AS total
FROM sample_counts;
```

The total is 16. PostgreSQL also provides transactions and savepoints for grouping changes and undoing work within a transaction, with defined behaviour rather than a replay request to a conversation. [The transaction tutorial](https://www.postgresql.org/docs/current/tutorial-transactions.html) describes those operations.
