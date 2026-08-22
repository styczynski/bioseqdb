/**
 * @file external/ebi/notification_paths.cppm
 * @brief EBI Search notification endpoint fragments.
 */
module;

#include <string_view>

export module bioseq.external.ebi:notification_paths;

export namespace bioseq::external::ebi::descriptor {

/** @brief Log notification path entry. */
inline constexpr std::string_view log_path = R"bioseq_openapi(  "/log":
    post:
      operationId: handleLogging
      requestBody:
        content:
          application/json:
            schema:
              "$ref": "#/components/schemas/FrontEndLogRequest"
      responses:
        default:
          description: default response
          content:
            application/json:
              schema:
                "$ref": "#/components/schemas/FrontEndLogResponse"
)bioseq_openapi";

/** @brief Mail notification path entry. */
inline constexpr std::string_view mail_path = R"bioseq_openapi(  "/mail":
    post:
      operationId: sendEmail
      requestBody:
        content:
          application/json:
            schema:
              "$ref": "#/components/schemas/EmailRequest"
      responses:
        default:
          description: default response
          content:
            application/json:
              schema:
                "$ref": "#/components/schemas/EmailResponse"
)bioseq_openapi";

} // namespace bioseq::external::ebi::descriptor
