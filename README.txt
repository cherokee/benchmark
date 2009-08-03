Cherokee Benchmark FAQ
======================

- What does this program do?

  This tiny application performs very basic HTTP 1.1 benchmarks.

- Why did you write it?

  The Apache Benchmark (ab) is the de-facto standard for this sort of
  quick benchmarks. However it lacks support for some of the most
  basic and widespread HTTP features, such as Chunked-encoding.
  Basically, the problem is that ab doesn't support HTTP 1.1.

- Why C?

  At the beginning I wrote this application in Python, but then I
  realized that the current Python (2.6) threading support didn't
  allow the application to scale as it should. Therefore, I had to
  rewrite it in C.

- Compilation and Dependencies

  It depends on Pthread, libcurl and libcherokee-base. Why? Well, just
  because it was the most convenient way for me to get it working in a
  couple of hours (so we could continue performing the benchmarks we
  were working on).

- What's the future of this application?

  I have no idea. Right now it's the best tool I know for performing
  quick benchmarks on HTTP/1.1. As long as we use it for our tests it
  ought to be fairly well supported.


Alvaro Lopez Ortega <alvaro@alobbs.com>, Aug 2009.
