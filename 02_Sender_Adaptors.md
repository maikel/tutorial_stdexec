# Introduction

This article is part of a series of articles that I write about topics related to asynchronous programming with senders and receivers in C++.

In the first article we have seen multiple sender factories, such as `just_42`, `get_stop_token`, and two asynchronous schedulers.
Authoring sender factories is relatively beginner-friendly because they don't require you to do "complicated" type transformations on completion signatures.
They also don't require you to write the receiver part of an algorithm and sender factories act merely as consumers of stop tokens and potentially other properties that receivers provide.

In the present article I want to focus on the second category of sender algorithms: *sender adaptors*, instead.

In this part of my series