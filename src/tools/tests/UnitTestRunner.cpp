#include <tools/UnitTest.h>

using namespace tools;

int
main( int argc, char** argv )
{
#if 0
    tools::impl::UnitTestMain( argc, argv );
#else // 0
    AutoDispose<> envLifetime;
    Environment * env = NewSimpleEnvironment(envLifetime, "test");
    TOOLS_ASSERT(!!env);
    impl::Service * twoStage = /*env->get<impl::Service>()*/nullptr;
    if (!!twoStage) {
        AutoDispose<Request> startReq(twoStage->start());
        auto err(runRequestSynchronously(startReq));
        TOOLS_ASSERT(!err);
    }
    auto mgr = env->get<tools::unittest::impl::Management>();
    if (argc == 1) {
        mgr->run(StringIdNull());
    } else {
        if (StringId(argv[1]) == "-list") {
            if (argc > 2) {
                mgr->list(argv[2]);
            } else {
                mgr->list(StringIdNull());
            }
        } else {
            for (int i = 1; i != argc; ++i) {
                // Run the specified tests
                if (mgr->run(argv[i]) == 0) {
                    // TODO: log this
                    fprintf(stderr, "No tests found matching %s.", argv[i]);
                    TOOLS_ASSERT(!"Could not find test");
                }
            }
        }
    }
    if (!!twoStage) {
        AutoDispose<Request> stopReq(twoStage->stop());
        auto err(runRequestSynchronously(stopReq));
        TOOLS_ASSERT(!err);
    }
#endif // 0
}
