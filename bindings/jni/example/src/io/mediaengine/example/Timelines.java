/*
 * Timelines — shared JSON fixtures for the JVM-side demos
 * (Run, Cancel, ProgressThrow, FrameFetch). Pre-extraction the
 * passthrough timeline JSON template lived inline in 4 demos
 * (cycle 97 audit: `grep -c 'schemaVersion' bindings/jni/
 * example/src/io/mediaengine/example/*.java` returned 4
 * identical copies). A schema-field rename or a bug in the
 * template would have meant 4 sites to fix — this helper is
 * the single source of truth.
 *
 * Demos call `Timelines.passthrough("file://" + sourcePath)`.
 * For new test scenarios that need a different shape (e.g.
 * multi-track for a future RenderCompose demo), add a
 * sibling factory rather than parameterizing this one — keeps
 * each fixture's intent visible at the call site.
 */
package io.mediaengine.example;

public final class Timelines {

    private Timelines() {}

    /** 2-second single-clip passthrough timeline over `uri`. Frame
     *  rate / resolution are nominal — the passthrough sink ignores
     *  them and copies the source's stream params verbatim. */
    public static String passthrough(String uri) {
        return ""
            + "{\n"
            + "  \"schemaVersion\": 1,\n"
            + "  \"frameRate\":  {\"num\":30,\"den\":1},\n"
            + "  \"resolution\": {\"width\":160,\"height\":120},\n"
            + "  \"colorSpace\": {\"primaries\":\"bt709\",\"transfer\":\"bt709\","
                                + "\"matrix\":\"bt709\",\"range\":\"limited\"},\n"
            + "  \"assets\": [{\"id\":\"a0\",\"uri\":\"" + uri + "\"}],\n"
            + "  \"compositions\": [{\"id\":\"main\",\"tracks\":[{\n"
            + "    \"id\":\"v0\",\"kind\":\"video\",\"clips\":[{\n"
            + "      \"id\":\"c0\",\"type\":\"video\",\"assetId\":\"a0\",\n"
            + "      \"timeRange\":  {\"start\":{\"num\":0,\"den\":1},\"duration\":{\"num\":2,\"den\":1}},\n"
            + "      \"sourceRange\":{\"start\":{\"num\":0,\"den\":1},\"duration\":{\"num\":2,\"den\":1}}\n"
            + "    }]\n"
            + "  }]}],\n"
            + "  \"output\": {\"compositionId\":\"main\"}\n"
            + "}\n";
    }
}
