"""Wait for something observable instead of sleeping a fixed time."""
import time


class WaitTimeout(AssertionError):
    pass


def wait_until(predicate, deadline=30, poll=0.1, what='the condition'):
    """Calls predicate every poll seconds until it returns something true, and returns that.
    After deadline seconds raises WaitTimeout naming what it waited for and the last value."""
    end = time.monotonic() + deadline
    last = None
    while True:
        last = predicate()
        if last:
            return last
        if time.monotonic() >= end:
            raise WaitTimeout(f'timed out after {deadline} s waiting for {what}; last value {last!r}')
        time.sleep(poll)


def wait_stable(sample, deadline=30, poll=0.2, count=2, what='a steady value'):
    """Calls sample until it returns the same value count times in a row, and returns it."""
    seen = []

    def steady():
        value = sample()
        seen.append(value)
        del seen[:-count]
        return len(seen) == count and all(v == value for v in seen)
    wait_until(steady, deadline, poll, what)
    return seen[-1]
