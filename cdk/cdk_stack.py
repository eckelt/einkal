from aws_cdk import (
    App,
    Stack,
    aws_lambda as _lambda,
    aws_apigatewayv2 as apigw,
    aws_apigatewayv2_integrations as integrations,
    aws_s3 as s3,
    aws_events as events,
    aws_events_targets as targets,
    Duration,
)
from constructs import Construct

class CalendarLambdaStack(Stack):
    def __init__(self, scope: Construct, construct_id: str, **kwargs):
        super().__init__(scope, construct_id, **kwargs)

        # S3 Bucket für die Ergebnisse
        bucket = s3.Bucket(self, "CalendarBucket")

        # Lambda-Funktion aus cal.py
        calendar_lambda = _lambda.Function(
            self, "CalendarLambda",
            runtime=_lambda.Runtime.PYTHON_3_11,
            handler="lambda_cal_handler.handler",
            code=_lambda.Code.from_asset("../lambda/"),  # lambda_cal_handler.py liegt im Projektroot
            environment={
                "BUCKET_NAME": bucket.bucket_name,
            },
            timeout=Duration.minutes(5),
        )

        bucket.grant_write(calendar_lambda)

        # Regelmäßige Ausführung (z.B. jede Stunde)
        rule = events.Rule(
            self, "ScheduleRule",
            schedule=events.Schedule.rate(Duration.hours(1)),
        )
        rule.add_target(targets.LambdaFunction(calendar_lambda))
        
        # HTTP API Gateway für die Lambda-Funktion
        http_api = apigw.HttpApi(self, "CalendarHttpApi",
            default_integration=integrations.HttpLambdaIntegration("LambdaIntegration", calendar_lambda)
        )
# App-Initialisierung und Synthese
app = App()
CalendarLambdaStack(app, "CalendarLambdaStack")
app.synth()
